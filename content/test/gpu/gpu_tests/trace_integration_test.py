# Copyright 2016 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import sys

from gpu_tests import gpu_integration_test
from gpu_tests import path_util
from gpu_tests import pixel_test_pages
from gpu_tests import trace_test_expectations

from telemetry.timeline import model as model_module
from telemetry.timeline import tracing_config

gpu_relative_path = "content/test/data/gpu/"

data_paths = [os.path.join(
                  path_util.GetChromiumSrcDir(), gpu_relative_path),
              os.path.join(
                  path_util.GetChromiumSrcDir(), 'media', 'test', 'data')]

webgl_test_harness_script = r"""
  var domAutomationController = {};

  domAutomationController._finished = false;

  domAutomationController.send = function(msg) {
    // Issue a read pixel to synchronize the gpu process to ensure
    // the asynchronous category enabling is finished.
    var temp_canvas = document.createElement("canvas")
    temp_canvas.width = 1;
    temp_canvas.height = 1;
    var temp_gl = temp_canvas.getContext("experimental-webgl") ||
                  temp_canvas.getContext("webgl");
    if (temp_gl) {
      temp_gl.clear(temp_gl.COLOR_BUFFER_BIT);
      var id = new Uint8Array(4);
      temp_gl.readPixels(0, 0, 1, 1, temp_gl.RGBA, temp_gl.UNSIGNED_BYTE, id);
    } else {
      console.log('Failed to get WebGL context.');
    }

    domAutomationController._finished = true;
  }

  window.domAutomationController = domAutomationController;
"""

basic_test_harness_script = r"""
  var domAutomationController = {};

  domAutomationController._proceed = false;

  domAutomationController._readyForActions = false;
  domAutomationController._succeeded = false;
  domAutomationController._finished = false;

  domAutomationController.send = function(msg) {
    domAutomationController._proceed = true;
    let lmsg = msg.toLowerCase();
    if (lmsg == "ready") {
      domAutomationController._readyForActions = true;
    } else {
      domAutomationController._finished = true;
      if (lmsg == "success") {
        domAutomationController._succeeded = true;
      } else {
        domAutomationController._succeeded = false;
      }
    }
  }

  window.domAutomationController = domAutomationController;
"""

# Presentation mode enums match DXGI_FRAME_PRESENTATION_MODE
_SWAP_CHAIN_PRESENTATION_MODE_COMPOSED = 0
_SWAP_CHAIN_PRESENTATION_MODE_OVERLAY = 1
_SWAP_CHAIN_PRESENTATION_MODE_NONE = 2
_SWAP_CHAIN_PRESENTATION_MODE_COMPOSITION_FAILURE = 3
# The following is defined for Chromium testing internal use.
_SWAP_CHAIN_GET_FRAME_STATISTICS_MEDIA_FAILED = -1

# Pixel format enums match OverlayFormat in config/gpu/gpu_info.h
_SWAP_CHAIN_PIXEL_FORMAT_BGRA = 0
_SWAP_CHAIN_PIXEL_FORMAT_YUY2 = 1
_SWAP_CHAIN_PIXEL_FORMAT_NV12 = 2

_GET_STATISTICS_EVENT_NAME = 'GetFrameStatisticsMedia'
_SWAP_CHAIN_PRESENT_EVENT_NAME = 'SwapChain::Present'


class TraceIntegrationTest(gpu_integration_test.GpuIntegrationTest):
  """Tests GPU traces are plumbed through properly.

  Also tests that GPU Device traces show up on devices that support them."""

  @classmethod
  def Name(cls):
    return 'trace_test'

  @classmethod
  def GenerateGpuTests(cls, options):
    # Include the device level trace tests, even though they're
    # currently skipped on all platforms, to give a hint that they
    # should perhaps be enabled in the future.
    for p in pixel_test_pages.DefaultPages('TraceTest'):
      yield (p.name, gpu_relative_path + p.url,
             {'browser_args': [],
              'category': cls._DisabledByDefaultTraceCategory('gpu.service'),
              'test_harness_script': webgl_test_harness_script,
              'finish_js_condition': 'domAutomationController._finished',
              'success_eval_func': 'CheckGLCategory'})
    for p in pixel_test_pages.DefaultPages('DeviceTraceTest'):
      yield (p.name, gpu_relative_path + p.url,
             {'browser_args': [],
              'category': cls._DisabledByDefaultTraceCategory('gpu.device'),
              'test_harness_script': webgl_test_harness_script,
              'finish_js_condition': 'domAutomationController._finished',
              'success_eval_func': 'CheckGLCategory'})
    for p in pixel_test_pages.DirectCompositionPages('VideoPathTraceTest'):
      yield (p.name, gpu_relative_path + p.url,
             {'browser_args': p.browser_args,
              'category': cls._DisabledByDefaultTraceCategory('gpu.service'),
              'test_harness_script': basic_test_harness_script,
              'finish_js_condition': 'domAutomationController._finished',
              'success_eval_func': 'CheckVideoPath',
              'other_args': p.other_args})
    for p in pixel_test_pages.DirectCompositionPages('OverlayModeTraceTest'):
      if p.other_args and p.other_args.get('video_is_rotated', False):
        # For all drivers we tested, when a video is rotated, frames won't
        # be promoted to hardware overlays.
        continue
      yield (p.name, gpu_relative_path + p.url,
             {'browser_args': p.browser_args,
              'category': cls._DisabledByDefaultTraceCategory('gpu.service'),
              'test_harness_script': basic_test_harness_script,
              'finish_js_condition': 'domAutomationController._finished',
              'success_eval_func': 'CheckOverlayMode',
              'other_args': p.other_args})

  def RunActualGpuTest(self, test_path, *args):
    test_params = args[0]
    assert 'browser_args' in test_params
    assert 'category' in test_params
    assert 'test_harness_script' in test_params
    assert 'finish_js_condition' in test_params
    browser_args = test_params['browser_args']
    category = test_params['category']
    test_harness_script = test_params['test_harness_script']
    finish_js_condition = test_params['finish_js_condition']
    success_eval_func = test_params['success_eval_func']
    other_args = test_params.get('other_args', None)

    # The version of this test in the old GPU test harness restarted
    # the browser after each test, so continue to do that to match its
    # behavior.
    self.RestartBrowserWithArgs(self._AddDefaultArgs(browser_args))

    # Set up tracing.
    config = tracing_config.TracingConfig()
    config.chrome_trace_config.category_filter.AddExcludedCategory('*')
    config.chrome_trace_config.category_filter.AddDisabledByDefault(category)
    config.enable_chrome_trace = True
    tab = self.tab
    tab.browser.platform.tracing_controller.StartTracing(config, 60)

    # Perform page navigation.
    url = self.UrlOfStaticFilePath(test_path)
    tab.Navigate(url, script_to_evaluate_on_commit=test_harness_script)
    tab.action_runner.WaitForJavaScriptCondition(
        finish_js_condition, timeout=30)

    # Stop tracing.
    timeline_data = tab.browser.platform.tracing_controller.StopTracing()

    # Evaluate success.
    if success_eval_func:
      timeline_model = model_module.TimelineModel(timeline_data)
      event_iter = timeline_model.IterAllEvents(
          event_type_predicate=timeline_model.IsSliceOrAsyncSlice)
      prefixed_func_name = '_EvaluateSuccess_' + success_eval_func
      getattr(self, prefixed_func_name)(category, event_iter, other_args)

  @classmethod
  def _CreateExpectations(cls):
    return trace_test_expectations.TraceTestExpectations()

  @classmethod
  def SetUpProcess(cls):
    super(TraceIntegrationTest, cls).SetUpProcess()
    path_util.SetupTelemetryPaths()
    cls.CustomizeBrowserArgs(cls._AddDefaultArgs([]))
    cls.StartBrowser()
    cls.SetStaticServerDirs(data_paths)

  @staticmethod
  def _AddDefaultArgs(browser_args):
    # All tests receive the following options.
    return [
      '--enable-logging',
      '--enable-experimental-web-platform-features'] + browser_args

  def _GetOverlayBotConfigHelper(self):
    system_info = self.browser.GetSystemInfo()
    if not system_info:
      raise Exception("Browser doesn't support GetSystemInfo")
    gpu = system_info.gpu.devices[0]
    if not gpu:
      raise Exception("System Info doesn't have a gpu")
    os_version_name = self.browser.platform.GetOSVersionName()
    return self.GetOverlayBotConfig(
        os_version_name, gpu.vendor_id, gpu.device_id)

  @staticmethod
  def _SwapChainPixelFormatToStr(pixel_format):
    if pixel_format == _SWAP_CHAIN_PIXEL_FORMAT_BGRA:
      return 'BGRA'
    if pixel_format == _SWAP_CHAIN_PIXEL_FORMAT_YUY2:
      return 'YUY2'
    if pixel_format == _SWAP_CHAIN_PIXEL_FORMAT_NV12:
      return 'NV12'
    return str(pixel_format)

  @staticmethod
  def _SwapChainPresentationModeToStr(presentation_mode):
    if presentation_mode == _SWAP_CHAIN_PRESENTATION_MODE_COMPOSED:
      return 'COMPOSED'
    if presentation_mode == _SWAP_CHAIN_PRESENTATION_MODE_OVERLAY:
      return 'OVERLAY'
    if presentation_mode == _SWAP_CHAIN_PRESENTATION_MODE_NONE:
      return 'NONE'
    if presentation_mode == _SWAP_CHAIN_PRESENTATION_MODE_COMPOSITION_FAILURE:
      return 'COMPOSITION_FAILURE'
    if presentation_mode == _SWAP_CHAIN_GET_FRAME_STATISTICS_MEDIA_FAILED:
      return 'GET_STATISTICS_FAILED'
    return str(presentation_mode)

  @staticmethod
  def _SwapChainPresentationModeListToStr(presentation_mode_list):
    list_str = None
    for mode in presentation_mode_list:
      mode_str = TraceIntegrationTest._SwapChainPresentationModeToStr(mode)
      if list_str is None:
        list_str = mode_str
      else:
        list_str = '%s,%s' % (list_str, mode_str)
    return '[%s]' % list_str

  @staticmethod
  def _DisabledByDefaultTraceCategory(category):
    return 'disabled-by-default-%s' % category

  #########################################
  # The test success evaluation functions

  def _EvaluateSuccess_CheckGLCategory(self, category, event_iterator,
                                       other_args):
    for event in event_iterator:
      if (event.category == category and
          event.args.get('gl_category', None) == 'gpu_toplevel'):
        break
    else:
      self.fail('Trace markers for GPU category %s were not found' % category)

  def _EvaluateSuccess_CheckVideoPath(self, category, event_iterator,
                                      other_args):
    """Verifies Chrome goes down the code path as expected.

    Depending on whether hardware overlays are supported or not, which formats
    are supported in overlays, whether video is downscaled or not, whether
    video is rotated or not, Chrome's video presentation code path can be
    different.
    """
    os_name = self.browser.platform.GetOSName()
    assert os_name and os_name.lower() == 'win'

    # Calculate expectations.
    if other_args is None:
      other_args = {}
    expect_yuy2 = other_args.get('expect_yuy2', False)
    zero_copy = other_args.get('zero_copy', False)

    overlay_bot_config = self.GetOverlayBotConfig()
    if overlay_bot_config is None:
      self.fail('Overlay bot config can not be determined')
    assert overlay_bot_config.get('direct_composition', False)

    expected_pixel_format = _SWAP_CHAIN_PIXEL_FORMAT_NV12
    supports_nv12_overlays = False
    if overlay_bot_config.get('supports_overlays', False):
      supports_yuy2_overlays = False
      if overlay_bot_config.get('overlay_cap_yuy2', 'NONE') != 'NONE':
        supports_yuy2_overlays = True
      if overlay_bot_config.get('overlay_cap_nv12', 'NONE') != 'NONE':
        supports_nv12_overlays = True
      assert supports_yuy2_overlays or supports_nv12_overlays
      if expect_yuy2 or not supports_nv12_overlays:
        expected_pixel_format = _SWAP_CHAIN_PIXEL_FORMAT_YUY2
    if not supports_nv12_overlays:
      zero_copy = False

    # Verify expectations through captured trace events.
    for event in event_iterator:
      if event.category != category:
        continue
      if event.name != _SWAP_CHAIN_PRESENT_EVENT_NAME:
        continue
      detected_pixel_format = event.args.get('PixelFormat', None)
      if detected_pixel_format is None:
        self.fail('PixelFormat is missing from event %s' %
                  _SWAP_CHAIN_PRESENT_EVENT_NAME)
      if expected_pixel_format != detected_pixel_format:
        self.fail('SwapChain pixel format mismatch, expected %s got %s' %
            (TraceIntegrationTest._SwapChainPixelFormatToStr(
                 expected_pixel_format),
             TraceIntegrationTest._SwapChainPixelFormatToStr(
                 detected_pixel_format)))
      detected_zero_copy = event.args.get('ZeroCopy', None)
      if detected_zero_copy is None:
        self.fail('ZeroCopy is missing from event %s' %
                  _SWAP_CHAIN_PRESENT_EVENT_NAME)
      if zero_copy != detected_zero_copy:
        self.fail('ZeroCopy mismatch, expected %s got %s' %
                  (zero_copy, detected_zero_copy))
      break
    else:
      self.fail('Events with name %s were not found' %
                _SWAP_CHAIN_PRESENT_EVENT_NAME)

  def _EvaluateSuccess_CheckOverlayMode(self, category, event_iterator,
                                        other_args):
    """Verifies video frames are promoted to overlays when supported."""
    os_name = self.browser.platform.GetOSName()
    assert os_name and os_name.lower() == 'win'

    overlay_bot_config = self.GetOverlayBotConfig()
    if overlay_bot_config is None:
      self.fail('Overlay bot config can not be determined')
    assert overlay_bot_config.get('direct_composition', False)

    expected_presentation_mode = _SWAP_CHAIN_PRESENTATION_MODE_COMPOSED
    if overlay_bot_config.get('supports_overlays', False):
      expected_presentation_mode = _SWAP_CHAIN_PRESENTATION_MODE_OVERLAY

    presentation_mode_history = []
    for event in event_iterator:
      if event.category != category:
        continue
      if event.name != _GET_STATISTICS_EVENT_NAME:
        continue
      detected_presentation_mode = event.args.get('CompositionMode', None)
      if detected_presentation_mode is None:
        self.fail('PresentationMode is missing from event %s' %
                  _GET_STATISTICS_EVENT_NAME)
      presentation_mode_history.append(detected_presentation_mode)
    valid_entry_found = False
    for index in range(len(presentation_mode_history)):
      mode = presentation_mode_history[index]
      if (mode == _SWAP_CHAIN_PRESENTATION_MODE_NONE or
          mode == _SWAP_CHAIN_GET_FRAME_STATISTICS_MEDIA_FAILED):
        # Be more tolerant to avoid test flakiness
        continue
      if mode != expected_presentation_mode:
        if index >= len(presentation_mode_history) // 2:
          # Be more tolerant for the first half frames in non-overlay mode.
          self.fail('SwapChain presentation mode mismatch, expected %s got %s' %
              (TraceIntegrationTest._SwapChainPresentationModeToStr(
                   expected_presentation_mode),
               TraceIntegrationTest._SwapChainPresentationModeListToStr(
                   presentation_mode_history)))
      valid_entry_found = True
    if not valid_entry_found:
      self.fail('No valid frame statistics being collected: %s',
          TraceIntegrationTest._SwapChainPresentationModeListToStr(
              presentation_mode_history))


def load_tests(loader, tests, pattern):
  del loader, tests, pattern  # Unused.
  return gpu_integration_test.LoadAllTestsInModule(sys.modules[__name__])
