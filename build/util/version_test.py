# Copyright 2019 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import unittest

import mock
import version


def _ReplaceArgs(args, *replacements):
  new_args = args[:]
  for flag, val in replacements:
    flag_index = args.index(flag)
    new_args[flag_index + 1] = val
  return new_args


class _VersionTest(unittest.TestCase):
  """Unittests for the version module.
  """

  _CHROME_VERSION_FILE = os.path.join(
      os.path.dirname(__file__), os.pardir, os.pardir, 'chrome', 'VERSION')

  _SCRIPT = os.path.join(os.path.dirname(__file__), 'version.py')

  _EXAMPLE_VERSION = {
      'MAJOR': '74',
      'MINOR': '0',
      'BUILD': '3720',
      'PATCH': '0',
  }

  _EXAMPLE_TEMPLATE = (
      'full = "@MAJOR@.@MINOR@.@BUILD@.@PATCH@" '
      'major = "@MAJOR@" minor = "@MINOR@" '
      'build = "@BUILD@" patch = "@PATCH@" version_id = @VERSION_ID@ ')

  _ANDROID_CHROME_VARS = [
      'chrome_version_code',
      'chrome_modern_version_code',
      'monochrome_version_code',
      'trichrome_version_code',
      'webview_version_code',
  ]

  _EXAMPLE_ANDROID_TEMPLATE = (
      _EXAMPLE_TEMPLATE + ''.join(
          ['%s = "@%s@" ' % (el, el.upper()) for el in _ANDROID_CHROME_VARS]))

  _EXAMPLE_ARGS = [
      '-f',
      _CHROME_VERSION_FILE,
      '-t',
      _EXAMPLE_TEMPLATE,
  ]

  _EXAMPLE_ANDROID_ARGS = _ReplaceArgs(_EXAMPLE_ARGS,
                                       ['-t', _EXAMPLE_ANDROID_TEMPLATE]) + [
                                           '-a',
                                           'arm',
                                           '--os',
                                           'android',
                                       ]

  @staticmethod
  def _RunBuildOutput(new_version_values={},
                      get_new_args=lambda old_args: old_args):
    """Parameterized helper method for running the main testable method in
    version.py.

    Keyword arguments:
    new_version_values -- dict used to update _EXAMPLE_VERSION
    get_new_args -- lambda for updating _EXAMPLE_ANDROID_ARGS
    """

    with mock.patch('version.FetchValuesFromFile') as \
        fetch_values_from_file_mock:

      fetch_values_from_file_mock.side_effect = (lambda values, file :
          values.update(
              dict(_VersionTest._EXAMPLE_VERSION, **new_version_values)))

      new_args = get_new_args(_VersionTest._EXAMPLE_ARGS)
      return version.BuildOutput(new_args)

  def testFetchValuesFromFile(self):
    """It returns a dict in correct format - { <str>: <str> }, to verify
    assumption of other tests that mock this function
    """
    result = {}
    version.FetchValuesFromFile(result, self._CHROME_VERSION_FILE)

    for key, val in result.iteritems():
      self.assertIsInstance(key, str)
      self.assertIsInstance(val, str)

  def testBuildOutputAndroid(self):
    """Assert it gives includes assignments of expected variables"""
    output = self._RunBuildOutput(
        get_new_args=lambda args: self._EXAMPLE_ANDROID_ARGS)
    contents = output['contents']

    self.assertRegexpMatches(contents, r'\bchrome_version_code = "\d+"\s')
    self.assertRegexpMatches(contents,
                             r'\bchrome_modern_version_code = "\d+"\s')
    self.assertRegexpMatches(contents, r'\bmonochrome_version_code = "\d+"\s')
    self.assertRegexpMatches(contents, r'\btrichrome_version_code = "\d+"\s')
    self.assertRegexpMatches(contents, r'\bwebview_version_code = "\d+"\s')

  def testBuildOutputAndroidChromeArchInput(self):
    """Assert it raises an exception when using an invalid architecture input"""
    new_args = _ReplaceArgs(self._EXAMPLE_ANDROID_ARGS, ['-a', 'foobar'])
    with self.assertRaises(SystemExit) as cm:
      self._RunBuildOutput(get_new_args=lambda args: new_args)

    self.assertEqual(cm.exception.code, 2)


if __name__ == '__main__':
  unittest.main()
