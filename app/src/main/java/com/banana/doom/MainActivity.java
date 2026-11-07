package com.banana.doom;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * Thin wrapper around NativeActivity: immersive fullscreen + keep screen on,
 * plus a hidden EditText that brings up the system keyboard so the native
 * game can ask for text (savegame names).
 */
public class MainActivity extends NativeActivity {
    /** Registered from C (textinput.c) with RegisterNatives. */
    private native void nativeTextChanged(String text);

    private EditText textInput;
    private InputMethodManager imm;
    private boolean textActive;
    private boolean settingText;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        super.onCreate(savedInstanceState);

        // NativeActivity asks for adjustResize; a resizing window would make the
        // game surface jump every time the keyboard opens. Never open it by itself.
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
                | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        createTextInput();
    }

    // ------------------------------------------------------------------
    // system keyboard
    // ------------------------------------------------------------------

    private void createTextInput() {
        textInput = new EditText(this);
        textInput.setBackgroundColor(Color.TRANSPARENT);
        textInput.setTextColor(Color.TRANSPARENT);
        textInput.setCursorVisible(false);
        textInput.setSingleLine(true);
        textInput.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS);
        textInput.setImeOptions(EditorInfo.IME_ACTION_DONE);
        textInput.setFocusable(false);               // only focusable while editing
        textInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable e) {
                if (!settingText && textActive) nativeTextChanged(toDoomText(e.toString()));
            }
        });
        // "Done" on the keyboard just closes it; the game's OK button saves.
        textInput.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override public boolean onEditorAction(TextView v, int actionId, android.view.KeyEvent ev) {
                closeKeyboard();
                return true;
            }
        });
        addContentView(textInput, new FrameLayout.LayoutParams(1, 1, Gravity.TOP | Gravity.START));
    }

    /** Called from native code (game thread): open the keyboard with this text. */
    public void showTextInput(final String initial, final int maxLen) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (textInput == null) return;
                textActive = true;
                textInput.setFilters(new InputFilter[] { new InputFilter.LengthFilter(Math.max(1, maxLen)) });
                settingText = true;
                textInput.setText(initial);
                textInput.setSelection(textInput.getText().length());
                settingText = false;
                textInput.setFocusable(true);
                textInput.setFocusableInTouchMode(true);
                textInput.requestFocus();
                showKeyboardWithRetry(5);
            }
        });
    }

    /** Called from native code (game thread): the name was confirmed or cancelled. */
    public void hideTextInput() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                textActive = false;
                closeKeyboard();
            }
        });
    }

    private void showKeyboardWithRetry(final int tries) {
        if (!textActive || textInput == null) return;
        textInput.post(new Runnable() {
            @Override public void run() {
                if (!textActive) return;
                imm.showSoftInput(textInput, 0);
                if (tries > 1 && !imm.isAcceptingText()) {
                    textInput.postDelayed(new Runnable() {
                        @Override public void run() { showKeyboardWithRetry(tries - 1); }
                    }, 100);
                }
            }
        });
    }

    private void closeKeyboard() {
        if (textInput == null) return;
        imm.hideSoftInputFromWindow(textInput.getWindowToken(), 0);
        textInput.setFocusable(false);
        textInput.clearFocus();
        hideSystemUi();
    }

    // Doom's menu font only has A-Z, digits and punctuation. Make Russian (and
    // Ukrainian) names readable by transliterating them; drop whatever is left.
    private static final String[] CYR = {
        "A","B","V","G","D","E","ZH","Z","I","J","K","L","M","N","O","P",
        "R","S","T","U","F","H","C","CH","SH","SCH","","Y","","E","YU","YA"
    };   // U+0410 .. U+042F

    private static String toDoomText(String raw) {
        String up = raw.toUpperCase(Locale.ROOT);
        StringBuilder sb = new StringBuilder(up.length() + 8);
        for (int i = 0; i < up.length(); i++) {
            char c = up.charAt(i);
            if (c >= 0x0410 && c <= 0x042F) sb.append(CYR[c - 0x0410]);
            else if (c == 0x0401) sb.append("YO");      // Ё
            else if (c == 0x0406) sb.append('I');       // І
            else if (c == 0x0407) sb.append("YI");      // Ї
            else if (c == 0x0404) sb.append("YE");      // Є
            else if (c == 0x0490) sb.append('G');       // Ґ
            else if (c >= ' ' && c <= '_') sb.append(c);
        }
        return sb.toString();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        Window w = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }
}
