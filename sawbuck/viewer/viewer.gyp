# Copyright 2009 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

{
  'variables': {
    'chromium_code': 1,
  },
  'includes': [
    '../../build/common.gypi',
  ],
  'target_defaults': {
    'include_dirs': [
      '../..',
      '../../chrome/third_party/wtl/include',
    ],
    'defines': [
      '_WTL_NO_CSTRING',
    ],
  },
  'targets': [
    {
      'target_name': 'sawbuck_version',
      'type': 'none',
      'msvs_cygwin_shell': 0,
      'sources': [
        'version.gen.template',
      ],
      'actions': [
        {
          'action_name': 'make_version_gen',
          'inputs': [
            '../tools/template_replace.py',
            '../VERSION',
            'version.gen.template',
          ],
          'outputs': [
            '<(SHARED_INTERMEDIATE_DIR)/version.gen',
          ],
          'action': [
            'python',
            '../tools/template_replace.py',
            '--input', 'version.gen.template',
            '--output', '<(SHARED_INTERMEDIATE_DIR)/version.gen',
            '../VERSION',
          ],
          'process_outputs_as_sources': 1,
        },
      ],
      'all_dependent_settings': {
        'include_dirs': [
          '<(SHARED_INTERMEDIATE_DIR)',
        ],
      },
    },
    {
      'target_name': 'log_view_lib',
      'type': 'static_library',
      'sources': [
        'provider_dialog.cc',
        'provider_dialog.h',
        'kernel_log_consumer.cc',
        'kernel_log_consumer.h',
        'log_consumer.h',
        'log_consumer.cc',
        'log_viewer.h',
        'log_viewer.cc',
        'log_list_view.h',
        'log_list_view.cc',
        'stack_trace_list_view.h',
        'stack_trace_list_view.cc',
        'viewer_window.cc',
        'viewer_window.h',
      ],
      'dependencies': [
        '../sym_util/sym_util.gyp:sym_util',
        '../../base/base.gyp:base',
        '../../third_party/pcre/pcre.gyp:pcre',
      ],
    },
    {
      'target_name': 'Sawbuck',
      'type': 'executable',
      'sources': [
        'resource.h',
        'viewer_module.cc',
        'viewer_module.h',
        'viewer.rc',
        'version.rc',
      ],
      'dependencies': [
        'log_view_lib',
        'sawbuck_version',
        '../../base/base.gyp:base',
      ],
      'msvs_settings': {
        'VCLinkerTool': {
          'SubSystem': 2,
          'UACExecutionLevel': 2,
        },
      },
    },
    {
      'target_name': 'test_logger',
      'type': 'executable',
      'sources': [
        'test_logger.cc',
      ],
      'dependencies': [
        '../../base/base.gyp:base',
      ],
    },
  ]
}
