// The organ currently loaded, shared by the screens.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Map;

public final class OrganState {
  public static JSONObject description;
  public static String path;

  private OrganState() {}

  public static boolean isLoaded() {
    return description != null && description.has("name");
  }

  /** Reads the engine description after a load and informs the MIDI router */
  public static void refresh(MidiMapping mapping) throws JSONException {
    description = new JSONObject(NativeEngine.text(NativeEngine.describe()));

    Map<Integer, Integer> byOdf = new HashMap<>();
    JSONArray manuals = description.optJSONArray("manuals");
    int fallback = -1;

    if (manuals != null) {
      for (int i = 0; i < manuals.length(); i++) {
        JSONObject m = manuals.getJSONObject(i);
        int odf = m.getInt("odf");

        byOdf.put(odf, m.getInt("id"));
        if (fallback < 0 && odf >= 1) fallback = m.getInt("id");
      }
      if (fallback < 0 && manuals.length() > 0) fallback = 0;
    }
    mapping.setOrgan(byOdf, fallback);
  }
}
