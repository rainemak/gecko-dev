# Copyright (c) 2011 Google Inc. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
{
  'targets': [
    {
      'target_name': 'dep_framework',
      'product_name': 'Dependency Framework',
      'type': 'shared_library',
      'mac_bundle': 1,
      'sources': [ 'empty.c', ],
    },
    {
      'target_name': 'test_app',
      'product_name': 'Test App Gyp',
      'type': 'executable',
      'mac_bundle': 1,
      'dependencies': [ 'dep_framework', ],
      'sources': [
        'TestApp/main.m',
        'TestApp/TestApp_Prefix.pch',
        'TestApp/TestAppAppDelegate.h',
        'TestApp/TestAppAppDelegate.m',
      ],
      'mac_bundle_resources': [
        'TestApp/English.lproj/InfoPlist.strings',
        'TestApp/English.lproj/MainMenu.xib',
      ],
      'link_settings': {
        'libraries': [
          '$(SDKROOT)/System/Library/Frameworks/Cocoa.framework',
        ],
      },
      'xcode_settings': {
        'INFOPLIST_FILE': 'TestApp/TestApp-Info.plist',
      },
    },
  ],
}
