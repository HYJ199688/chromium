// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.exportPath('extensions');

/**
 * The different states the activity log page can be in. Initial state is
 * LOADING because we call the activity log API whenever a user navigates to the
 * page. LOADED is the state where the API call has returned a successful
 * result.
 * @enum {string}
 */
const ActivityLogPageState = {
  LOADING: 'loading',
  LOADED: 'loaded'
};

cr.define('extensions', function() {
  'use strict';

  /** @interface */
  class ActivityLogDelegate {
    /**
     * @param {string} extensionId
     * @return {!Promise<!chrome.activityLogPrivate.ActivityResultSet>}
     */
    getExtensionActivityLog(extensionId) {}

    /**
     * @param {string} extensionId
     * @param {string} searchTerm
     * @return {!Promise<!chrome.activityLogPrivate.ActivityResultSet>}
     */
    getFilteredExtensionActivityLog(extensionId, searchTerm) {}

    /**
     * @param {!Array<string>} activityIds
     * @return {!Promise<void>}
     */
    deleteActivitiesById(activityIds) {}

    /**
     * @param {string} extensionId
     * @return {!Promise<void>}
     */
    deleteActivitiesFromExtension(extensionId) {}
  }

  /**
   * Content scripts activities do not have an API call, so we use the names of
   * the scripts executed (specified as a stringified JSON array in the args
   * field) as the keys for an activity group instead.
   * @private
   * @param {!chrome.activityLogPrivate.ExtensionActivity} activity
   * @return {!Array<string>}
   */
  function getActivityGroupKeysForContentScript_(activity) {
    assert(
        activity.activityType ===
        chrome.activityLogPrivate.ExtensionActivityType.CONTENT_SCRIPT);

    if (!activity.args) {
      return [];
    }

    const parsedArgs = JSON.parse(activity.args);
    assert(Array.isArray(parsedArgs), 'Invalid API data.');
    return /** @type {!Array<string>} */ (parsedArgs);
  }

  /**
   * Web request activities can have extra information which describes what the
   * web request does in more detail than just the api_call. This information
   * is in activity.other.webRequest and we use this to generate more activity
   * group keys if possible.
   * @private
   * @param {!chrome.activityLogPrivate.ExtensionActivity} activity
   * @return {!Array<string>}
   */
  function getActivityGroupKeysForWebRequest_(activity) {
    assert(
        activity.activityType ===
        chrome.activityLogPrivate.ExtensionActivityType.WEB_REQUEST);

    const apiCall = activity.apiCall;
    const other = activity.other;

    if (!other || !other.webRequest) {
      return [apiCall];
    }

    const webRequest = /** @type {!Object} */ (JSON.parse(other.webRequest));
    assert(typeof webRequest === 'object', 'Invalid API data');

    // If there is extra information in the other.webRequest object,
    // construct a group for each consisting of the API call and each object key
    // in other.webRequest. Otherwise we default to just the API call.
    return Object.keys(webRequest).length === 0 ?
        [apiCall] :
        Object.keys(webRequest).map(field => `${apiCall} (${field})`);
  }

  /**
   * Group activity log entries by a key determined from each entry. Usually
   * this would be the activity's API call though content script and web
   * requests have different keys. We currently assume that every API call
   * matches to one activity type.
   * @param {!Array<!chrome.activityLogPrivate.ExtensionActivity>}
   *     activityData
   * @return {!Map<string, !extensions.ActivityGroup>}
   */
  function groupActivities(activityData) {
    const groupedActivities = new Map();

    for (const activity of activityData) {
      const activityId = activity.activityId;
      const activityType = activity.activityType;
      const count = activity.count;
      const pageUrl = activity.pageUrl;

      const isContentScript = activityType ===
          chrome.activityLogPrivate.ExtensionActivityType.CONTENT_SCRIPT;
      const isWebRequest = activityType ===
          chrome.activityLogPrivate.ExtensionActivityType.WEB_REQUEST;

      let activityGroupKeys = [activity.apiCall];
      if (isContentScript) {
        activityGroupKeys = getActivityGroupKeysForContentScript_(activity);
      } else if (isWebRequest) {
        activityGroupKeys = getActivityGroupKeysForWebRequest_(activity);
      }

      for (const key of activityGroupKeys) {
        if (!groupedActivities.has(key)) {
          const activityGroup = {
            activityIds: new Set([activityId]),
            key,
            count,
            activityType,
            countsByUrl: pageUrl ? new Map([[pageUrl, count]]) : new Map(),
            expanded: false,
          };
          groupedActivities.set(key, activityGroup);
        } else {
          const activityGroup = groupedActivities.get(key);
          activityGroup.activityIds.add(activityId);
          activityGroup.count += count;

          if (pageUrl) {
            const currentCount = activityGroup.countsByUrl.get(pageUrl) || 0;
            activityGroup.countsByUrl.set(pageUrl, currentCount + count);
          }
        }
      }
    }

    return groupedActivities;
  }

  /**
   * Sort activities by the total count for each activity group key. Resolve
   * ties by the alphabetical order of the key.
   * @param {!Map<string, !extensions.ActivityGroup>} groupedActivities
   * @return {!Array<!extensions.ActivityGroup>}
   */
  function sortActivitiesByCallCount(groupedActivities) {
    return Array.from(groupedActivities.values()).sort(function(a, b) {
      if (a.count != b.count) {
        return b.count - a.count;
      }
      if (a.key < b.key) {
        return -1;
      }
      if (a.key > b.key) {
        return 1;
      }
      return 0;
    });
  }

  const ActivityLogHistory = Polymer({
    is: 'activity-log-history',

    properties: {
      /** @type {!string} */
      extensionId: String,

      /** @type {!extensions.ActivityLogDelegate} */
      delegate: Object,

      /**
       * An array representing the activity log. Stores activities grouped by
       * API call or content script name sorted in descending order of the call
       * count.
       * @private {!Array<!extensions.ActivityGroup>}
       */
      activityData_: {
        type: Array,
        value: () => [],
      },

      /** @private {ActivityLogPageState} */
      pageState_: {
        type: String,
        value: ActivityLogPageState.LOADING,
      },

      /** @private */
      lastSearch_: {
        type: String,
        value: '',
      },
    },

    listeners: {
      'delete-activity-log-item': 'deleteItem_',
    },

    /**
     * A promise resolver for any external files waiting for the
     * GetExtensionActivity API call to finish.
     * Currently only used for extension_settings_browsertest.cc
     * @private {PromiseResolver}
     */
    dataFetchedResolver_: null,

    /**
     * Expose only the promise of dataFetchedResolver_.
     * @return {!Promise<void>}
     */
    whenDataFetched: function() {
      return this.dataFetchedResolver_.promise;
    },

    /** @override */
    attached: function() {
      this.dataFetchedResolver_ = new PromiseResolver();
      this.refreshActivities_();
    },

    /**
     * @private
     * @return {boolean}
     */
    shouldShowEmptyActivityLogMessage_: function() {
      return this.pageState_ === ActivityLogPageState.LOADED &&
          this.activityData_.length === 0;
    },

    /**
     * @private
     * @return {boolean}
     */
    shouldShowLoadingMessage_: function() {
      return this.pageState_ === ActivityLogPageState.LOADING;
    },

    /**
     * @private
     * @return {boolean}
     */
    shouldShowActivities_: function() {
      return this.pageState_ === ActivityLogPageState.LOADED &&
          this.activityData_.length > 0;
    },

    /** @private */
    onClearActivitiesClick_: function() {
      this.delegate.deleteActivitiesFromExtension(this.extensionId).then(() => {
        this.processActivities_([]);
      });
    },

    /** @private */
    onMoreActionsClick_: function() {
      this.$$('cr-action-menu').showAt(assert(this.$$('cr-icon-button')));
    },

    /**
     * @private
     * @param {boolean} expanded
     */
    expandItems_: function(expanded) {
      this.activityData_.filter(item => item.countsByUrl.size > 0)
          .forEach((item, index) => {
            this.set(`activityData_.${index}.expanded`, expanded);
          });
      this.$$('cr-action-menu').close();
    },

    /** @private */
    onExpandAllClick_: function() {
      this.expandItems_(true);
    },

    /** @private */
    onCollapseAllClick_: function() {
      this.expandItems_(false);
    },

    /**
     * @private
     * @param {!CustomEvent<!Array<string>>} e
     */
    deleteItem_: function(e) {
      const activityIds = e.detail;
      this.delegate.deleteActivitiesById(activityIds).then(() => {
        // It is possible for multiple activities displayed to have the same
        // underlying activity ID. This happens when we split content script and
        // web request activities by fields other than their API call. For
        // consistency, we will re-fetch the activity log.
        this.refreshActivities_();
      });
    },

    /**
     * @private
     * @param {!Array<!chrome.activityLogPrivate.ExtensionActivity>}
     *     activityData
     */
    processActivities_: function(activityData) {
      this.pageState_ = ActivityLogPageState.LOADED;
      this.activityData_ =
          sortActivitiesByCallCount(groupActivities(activityData));
      if (!this.dataFetchedResolver_.isFulfilled) {
        this.dataFetchedResolver_.resolve();
      }
    },

    /**
     * @private
     * @return {!Promise<void>}
     */
    refreshActivities_: function() {
      if (this.lastSearch_ === '') {
        return this.getActivityLog_();
      }

      return this.getFilteredActivityLog_(this.lastSearch_);
    },

    /**
     * @private
     * @return {!Promise<void>}
     */
    getActivityLog_: function() {
      this.pageState_ = ActivityLogPageState.LOADING;
      return this.delegate.getExtensionActivityLog(this.extensionId)
          .then(result => {
            this.processActivities_(result.activities);
          });
    },

    /**
     * @private
     * @param {string} searchTerm
     * @return {!Promise<void>}
     */
    getFilteredActivityLog_: function(searchTerm) {
      this.pageState_ = ActivityLogPageState.LOADING;
      return this.delegate
          .getFilteredExtensionActivityLog(this.extensionId, searchTerm)
          .then(result => {
            this.processActivities_(result.activities);
          });
    },

    /**
     * @private
     * @param {!CustomEvent<string>} e
     */
    onSearchChanged_: function(e) {
      // Remove all whitespaces from the search term, as API call names and
      // urls should not contain any whitespace. As of now, only single term
      // search queries are allowed.
      const searchTerm = e.detail.replace(/\s+/g, '');
      if (searchTerm === this.lastSearch_) {
        return;
      }

      this.lastSearch_ = searchTerm;
      this.refreshActivities_();
    },
  });

  return {
    ActivityLogHistory: ActivityLogHistory,
    ActivityLogDelegate: ActivityLogDelegate,
  };
});
