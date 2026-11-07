/*
 * System soft keyboard for text entry (savegame names).
 *
 * NativeActivity has no text input of its own, so MainActivity.java owns a
 * tiny invisible EditText.  This file is the bridge:
 *
 *   engine  -> I_TextInputStart()/I_TextInputStop() -> MainActivity (JNI call)
 *   keyboard-> MainActivity.nativeTextChanged()     -> kbd_poll() (game thread)
 *
 * The text arrives on the Android UI thread, so it is parked behind a mutex
 * and handed to the menu from the game thread in kbd_poll().
 */
#include <jni.h>
#include <pthread.h>
#include <string.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#include "doomdef.h"
#include "i_system.h"
#include "m_menu.h"
#include "plat.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "DOOM", __VA_ARGS__)

static JavaVM   *g_vm;
static jobject   g_activity;            /* ANativeActivity::clazz (the activity) */
static jmethodID g_show, g_hide;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_text[128];
static int  g_dirty;

/* UI thread: the EditText content changed (already upper-cased/transliterated). */
static void JNICALL native_text_changed(JNIEnv *env, jobject thiz, jstring s)
{
    const char *u;
    (void)thiz;
    if (!s) return;
    u = (*env)->GetStringUTFChars(env, s, NULL);
    if (!u) return;
    pthread_mutex_lock(&g_mx);
    strncpy(g_text, u, sizeof(g_text) - 1);
    g_text[sizeof(g_text) - 1] = 0;
    g_dirty = 1;
    pthread_mutex_unlock(&g_mx);
    (*env)->ReleaseStringUTFChars(env, s, u);
}

void kbd_init(struct android_app *app)
{
    JNIEnv *env = NULL;
    jclass cls;
    static const JNINativeMethod natives[] = {
        { "nativeTextChanged", "(Ljava/lang/String;)V", (void *)native_text_changed },
    };

    g_vm = app->activity->vm;
    g_activity = app->activity->clazz;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK || !env) {
        LOG("kbd_init: AttachCurrentThread failed");
        return;
    }
    cls = (*env)->GetObjectClass(env, g_activity);
    if ((*env)->RegisterNatives(env, cls, natives, 1) != 0) {
        LOG("kbd_init: RegisterNatives failed");
        (*env)->ExceptionClear(env);
    }
    g_show = (*env)->GetMethodID(env, cls, "showTextInput", "(Ljava/lang/String;I)V");
    g_hide = (*env)->GetMethodID(env, cls, "hideTextInput", "()V");
    if (!g_show || !g_hide) {
        LOG("kbd_init: MainActivity.showTextInput/hideTextInput not found");
        (*env)->ExceptionClear(env);
        g_show = g_hide = NULL;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*g_vm)->DetachCurrentThread(g_vm);
}

/* Attach the calling thread if needed; returns env or NULL. */
static JNIEnv *get_env(int *attached)
{
    JNIEnv *env = NULL;
    jint r;
    *attached = 0;
    if (!g_vm) return NULL;
    r = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
        *attached = 1;
    } else if (r != JNI_OK) {
        return NULL;
    }
    return env;
}

void I_TextInputStart(const char *initial, int maxlen)
{
    JNIEnv *env;
    int attached;
    jstring js;

    if (!g_show) return;
    env = get_env(&attached);
    if (!env) return;

    pthread_mutex_lock(&g_mx);
    g_dirty = 0;                        /* drop stale text from a previous entry */
    pthread_mutex_unlock(&g_mx);

    js = (*env)->NewStringUTF(env, initial ? initial : "");
    (*env)->CallVoidMethod(env, g_activity, g_show, js, (jint)maxlen);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, js);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

void I_TextInputStop(void)
{
    JNIEnv *env;
    int attached;

    if (!g_hide) return;
    env = get_env(&attached);
    if (!env) return;
    (*env)->CallVoidMethod(env, g_activity, g_hide);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);

    pthread_mutex_lock(&g_mx);
    g_dirty = 0;
    pthread_mutex_unlock(&g_mx);
}

/* Game thread, every tic: hand pending keyboard text to the menu. */
void kbd_poll(void)
{
    char buf[sizeof(g_text)];
    int dirty;

    pthread_mutex_lock(&g_mx);
    dirty = g_dirty;
    if (dirty) {
        memcpy(buf, g_text, sizeof(buf));
        g_dirty = 0;
    }
    pthread_mutex_unlock(&g_mx);

    if (dirty) M_SaveStringSet(buf);
}
