# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
{
  'targets': [
    {
      'target_name': 'action_link',
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'assert',
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'cr',
      'dependencies': [
        '<(EXTERNS_GYP):chrome_send',
        'assert',
      ],
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'load_time_data',
      'dependencies': ['<(DEPTH)/third_party/jstemplate/compiled_resources2.gyp:jstemplate'],
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'event_tracker',
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'util',
      'dependencies': [
        '<(EXTERNS_GYP):chrome_send',
        'cr',
      ],
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'parse_html_subset',
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'i18n_template_no_process',
      'dependencies': [
        'load_time_data',
        '<(EXTERNS_GYP):pending_compiler_externs',
      ],
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'i18n_template',
      'dependencies': [
        'load_time_data',
        # Ideally, <include> would automatically import externs as well, but
        # it current doesn't and that sounds hard. Let's just kill <include>.
        '<(EXTERNS_GYP):pending_compiler_externs',
      ],
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
    {
      'target_name': 'i18n_behavior',
      'dependencies': [
        'load_time_data',
      ],
      'includes': ['../../../../third_party/closure_compiler/compile_js2.gypi'],
    },
  ],
}
