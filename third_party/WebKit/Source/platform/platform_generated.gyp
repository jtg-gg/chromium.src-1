#
# Copyright (C) 2013 Google Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

{
  'includes': [
    '../build/features.gypi',
    '../build/scripts/scripts.gypi',
    'platform_generated.gypi',
  ],
  'variables': {
    'conditions': [
      # TODO(kojii): The character_data_generator fails when cross-compile, so
      # we use a pre-generated copy in the tree until we fix or until we move
      # to gn. See crbug.com/581555
      ['OS=="android" or chromeos==1 or (target_arch!="ia32" and target_arch!="x64")', {
        'generate_character_data%': 0,
      }, {
        'generate_character_data%': 1,
      }],
    ],
  },

  'targets': [
    {
      'target_name': 'make_platform_generated',
      'type': 'none',
      'hard_dependency': 1,
      'dependencies': [
        'inspector_protocol/protocol.gyp:protocol_sources',
        'v8_inspector/v8_inspector.gyp:inspector_debugger_script',
        'v8_inspector/v8_inspector.gyp:inspector_injected_script',
      ],
      'conditions': [
        ['generate_character_data==1', {
          'dependencies': [
            'character_data_generator#host',
          ],
        }]
      ],
      'actions': [
        {
          'action_name': 'FontFamilyNames',
          'inputs': [
            '<@(make_names_files)',
            'fonts/FontFamilyNames.in',
          ],
          'outputs': [
            '<(blink_platform_output_dir)/FontFamilyNames.cpp',
            '<(blink_platform_output_dir)/FontFamilyNames.h',
          ],
          'action': [
            'python',
            '../build/scripts/make_names.py',
            'fonts/FontFamilyNames.in',
            '--output_dir',
            '<(blink_platform_output_dir)',
          ],
        },
        {
          'action_name': 'HTTPNames',
          'inputs': [
            '<@(make_names_files)',
            'network/HTTPNames.in',
          ],
          'outputs': [
            '<(blink_platform_output_dir)/HTTPNames.cpp',
            '<(blink_platform_output_dir)/HTTPNames.h',
          ],
          'action': [
            'python',
            '../build/scripts/make_names.py',
            'network/HTTPNames.in',
            '--output_dir',
            '<(blink_platform_output_dir)',
          ],
        },
        {
          'action_name': 'RuntimeEnabledFeatures',
          'inputs': [
            '<@(scripts_for_in_files)',
            '../build/scripts/make_runtime_features.py',
            'RuntimeEnabledFeatures.in',
            '../build/scripts/templates/RuntimeEnabledFeatures.cpp.tmpl',
            '../build/scripts/templates/RuntimeEnabledFeatures.h.tmpl',
          ],
          'outputs': [
            '<(blink_platform_output_dir)/RuntimeEnabledFeatures.cpp',
            '<(blink_platform_output_dir)/RuntimeEnabledFeatures.h',
          ],
          'action': [
            'python',
            '../build/scripts/make_runtime_features.py',
            'RuntimeEnabledFeatures.in',
            '--output_dir',
            '<(blink_platform_output_dir)',
          ],
        },
        {
          'action_name': 'ColorData',
          'inputs': [
            'ColorData.gperf',
          ],
          'outputs': [
            '<(blink_platform_output_dir)/ColorData.cpp',
          ],
          'action': [
            '<(gperf_exe)',
            '--key-positions=*',
            '-D', '-s', '2',
            '<@(_inputs)',
            '--output-file=<(blink_platform_output_dir)/ColorData.cpp',
          ],
        },
        {
          'action_name': 'CharacterData',
          'inputs': [
            'fonts/CharacterDataGenerator.cpp',
          ],
          'outputs': [
            '<(blink_platform_output_dir)/CharacterData.cpp',
          ],
          'conditions': [
            ['generate_character_data==1', {
              'action': [
                '<(PRODUCT_DIR)/character_data_generator',
                '<(blink_platform_output_dir)/CharacterData.cpp',
              ],
            }, {
              'action': [
                'cp',
                'fonts/CharacterData.cpp',
                '<(blink_platform_output_dir)/CharacterData.cpp',
              ],
            }]
          ],
        },
      ]
    },
    {
      'target_name': 'character_data_generator',
      'type': 'executable',
      'toolsets': ['host'],
      'sources': [
        'fonts/CharacterDataGenerator.cpp',
      ],
      'dependencies': [
        '<(DEPTH)/third_party/icu/icu.gyp:icuuc#host',
      ],
    },
  ],
}
