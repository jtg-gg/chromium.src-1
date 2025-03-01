// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * 'all-sites' is the polymer element for showing the list of all sites under
 * Site Settings.
 *
 * @group Chrome Settings Elements
 * @element all-sites
 */
Polymer({
  is: 'all-sites',

  behaviors: [SiteSettingsBehavior],

  properties: {
    /**
     * Preferences state.
     */
    prefs: {
      type: Object,
      notify: true,
    },

    /**
     * The current active route.
     */
    currentRoute: {
      type: Object,
      notify: true,
    },

    /**
     * The origin that was selected by the user in the dropdown list.
     */
    selectedOrigin: {
      type: String,
      notify: true,
    },
  },
});
