// Small helpers for building the screens in code.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

final class Ui {
  static final int WOOD = Color.rgb(0x5a, 0x3a, 0x1e);
  static final int WOOD_LIGHT = Color.rgb(0x8b, 0x5e, 0x34);
  static final int IVORY = Color.rgb(0xf3, 0xea, 0xd7);
  static final int STOP_ON = Color.rgb(0xf6, 0xe7, 0xb4);
  static final int STOP_OFF = Color.rgb(0x7a, 0x6a, 0x58);
  static final int RED = Color.rgb(0xb3, 0x2b, 0x1d);

  private Ui() {}

  static int dp(Context c, float v) {
    return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
  }

  static LinearLayout column(Context c) {
    LinearLayout l = new LinearLayout(c);

    l.setOrientation(LinearLayout.VERTICAL);
    return l;
  }

  static LinearLayout row(Context c) {
    LinearLayout l = new LinearLayout(c);

    l.setOrientation(LinearLayout.HORIZONTAL);
    l.setGravity(Gravity.CENTER_VERTICAL);
    return l;
  }

  static TextView text(Context c, String s, float sp, boolean isBold) {
    TextView t = new TextView(c);

    t.setText(s);
    t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
    t.setTextColor(IVORY);
    if (isBold) t.setTypeface(Typeface.DEFAULT_BOLD);
    return t;
  }

  static GradientDrawable rounded(int color, float radiusPx, int strokeColor) {
    GradientDrawable d = new GradientDrawable();

    d.setColor(color);
    d.setCornerRadius(radiusPx);
    if (strokeColor != 0) d.setStroke(2, strokeColor);
    return d;
  }

  static Button button(Context c, String label) {
    Button b = new Button(c);

    b.setText(label);
    b.setAllCaps(false);
    b.setTextColor(Color.BLACK);
    b.setBackground(rounded(IVORY, dp(c, 8), WOOD_LIGHT));
    b.setPadding(dp(c, 12), dp(c, 6), dp(c, 12), dp(c, 6));
    return b;
  }

  static LinearLayout.LayoutParams wrap(Context c, int marginDp) {
    LinearLayout.LayoutParams p =
        new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    int m = dp(c, marginDp);

    p.setMargins(m, m, m, m);
    return p;
  }

  static LinearLayout.LayoutParams fill(Context c, int marginDp) {
    LinearLayout.LayoutParams p =
        new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    int m = dp(c, marginDp);

    p.setMargins(m, m, m, m);
    return p;
  }

  static String formatSize(long bytes) {
    if (bytes >= 1024L * 1024 * 1024) return String.format("%.1f GB", bytes / (1024.0 * 1024 * 1024));
    return String.format("%.0f MB", bytes / (1024.0 * 1024));
  }
}
