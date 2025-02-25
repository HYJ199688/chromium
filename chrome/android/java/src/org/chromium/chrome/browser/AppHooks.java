// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser;

import android.app.Activity;
import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.support.annotation.Nullable;
import android.support.v4.content.ContextCompat;

import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailability;

import org.chromium.base.Callback;
import org.chromium.base.CommandLine;
import org.chromium.base.ContextUtils;
import org.chromium.base.VisibleForTesting;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.task.PostTask;
import org.chromium.chrome.browser.banners.AppDetailsDelegate;
import org.chromium.chrome.browser.customtabs.CustomTabsConnection;
import org.chromium.chrome.browser.externalauth.ExternalAuthUtils;
import org.chromium.chrome.browser.feedback.AsyncFeedbackSource;
import org.chromium.chrome.browser.feedback.FeedbackCollector;
import org.chromium.chrome.browser.feedback.FeedbackReporter;
import org.chromium.chrome.browser.feedback.FeedbackSource;
import org.chromium.chrome.browser.feedback.FeedbackSourceProvider;
import org.chromium.chrome.browser.firstrun.FreIntentCreator;
import org.chromium.chrome.browser.gsa.GSAHelper;
import org.chromium.chrome.browser.help.HelpAndFeedback;
import org.chromium.chrome.browser.historyreport.AppIndexingReporter;
import org.chromium.chrome.browser.init.ProcessInitializationHandler;
import org.chromium.chrome.browser.instantapps.InstantAppsHandler;
import org.chromium.chrome.browser.locale.LocaleManager;
import org.chromium.chrome.browser.metrics.VariationsSession;
import org.chromium.chrome.browser.multiwindow.MultiWindowUtils;
import org.chromium.chrome.browser.offlinepages.CCTRequestStatus;
import org.chromium.chrome.browser.omaha.RequestGenerator;
import org.chromium.chrome.browser.partnerbookmarks.PartnerBookmark;
import org.chromium.chrome.browser.partnerbookmarks.PartnerBookmarksProviderIterator;
import org.chromium.chrome.browser.partnercustomizations.PartnerBrowserCustomizations;
import org.chromium.chrome.browser.password_manager.GooglePasswordManagerUIProvider;
import org.chromium.chrome.browser.policy.PolicyAuditor;
import org.chromium.chrome.browser.preferences.LocationSettings;
import org.chromium.chrome.browser.rlz.RevenueStats;
import org.chromium.chrome.browser.services.AndroidEduOwnerCheckCallback;
import org.chromium.chrome.browser.signin.GoogleActivityController;
import org.chromium.chrome.browser.survey.SurveyController;
import org.chromium.chrome.browser.tab.AuthenticatorNavigationInterceptor;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.touchless.TouchlessUiController;
import org.chromium.chrome.browser.usage_stats.DigitalWellbeingClient;
import org.chromium.chrome.browser.webapps.GooglePlayWebApkInstallDelegate;
import org.chromium.chrome.browser.webauth.Fido2ApiHandler;
import org.chromium.chrome.browser.widget.FeatureHighlightProvider;
import org.chromium.components.download.DownloadCollectionBridge;
import org.chromium.components.signin.AccountManagerDelegate;
import org.chromium.components.signin.SystemAccountManagerDelegate;
import org.chromium.content_public.browser.RenderFrameHost;
import org.chromium.content_public.browser.UiThreadTaskTraits;
import org.chromium.policy.AppRestrictionsProvider;
import org.chromium.policy.CombinedPolicyProvider;
import org.chromium.services.service_manager.InterfaceRegistry;

import java.util.Collections;
import java.util.List;

/**
 * Base class for defining methods where different behavior is required by downstream targets.
 * The correct version of {@link AppHooksImpl} will be determined at compile time via build rules.
 * See http://crbug/560466.
 */
public abstract class AppHooks {
    private static AppHooksImpl sInstance;

    /**
     * Sets a mocked instance for testing.
     */
    @VisibleForTesting
    public static void setInstanceForTesting(AppHooksImpl instance) {
        sInstance = instance;
    }

    @CalledByNative
    public static AppHooks get() {
        if (sInstance == null) sInstance = new AppHooksImpl();
        return sInstance;
    }

    /**
     * Initiate AndroidEdu device check.
     * @param callback Callback that should receive the results of the AndroidEdu device check.
     */
    public void checkIsAndroidEduDevice(final AndroidEduOwnerCheckCallback callback) {
        PostTask.postTask(UiThreadTaskTraits.DEFAULT, () -> callback.onSchoolCheckDone(false));
    }

    /**
     * Perform platform-specific command line initialization.
     * @param instance CommandLine instance to be updated.
     */
    public void initCommandLine(CommandLine instance) {}

    /**
     * Inform platform of current display mode.
     * @param displayMode the new display mode (see WebDisplayMode)
     * @param activity the affected activity.
     */
    public void setDisplayModeForActivity(int displayMode, Activity activity) {}

    /**
     * Creates a new {@link AccountManagerDelegate}.
     * @return the created {@link AccountManagerDelegate}.
     */
    public AccountManagerDelegate createAccountManagerDelegate() {
        return new SystemAccountManagerDelegate();
    }

    /**
     * @return An instance of AppDetailsDelegate that can be queried about app information for the
     *         App Banner feature.  Will be null if one is unavailable.
     */
    public AppDetailsDelegate createAppDetailsDelegate() {
        return null;
    }

    /**
     * Creates a new {@link AppIndexingReporter}.
     * @return the created {@link AppIndexingReporter}.
     */
    public AppIndexingReporter createAppIndexingReporter() {
        return new AppIndexingReporter();
    }

    /**
     * Return a {@link AuthenticatorNavigationInterceptor} for the given {@link Tab}.
     * This can be null if there are no applicable interceptor to be built.
     */
    public AuthenticatorNavigationInterceptor createAuthenticatorNavigationInterceptor(Tab tab) {
        return null;
    }

    /**
     * @return An instance of {@link CustomTabsConnection}. Should not be called
     * outside of {@link CustomTabsConnection#getInstance()}.
     */
    public CustomTabsConnection createCustomTabsConnection() {
        return new CustomTabsConnection();
    }

    /**
     * Creates a new {@link SurveyController}.
     * @return The created {@link SurveyController}.
     */
    public SurveyController createSurveyController() {
        return new SurveyController();
    }

    /**
     * @return An instance of ExternalAuthUtils to be installed as a singleton.
     */
    public ExternalAuthUtils createExternalAuthUtils() {
        return new ExternalAuthUtils();
    }

    /**
     * @return An instance of {@link FeedbackReporter} to report feedback.
     */
    public FeedbackReporter createFeedbackReporter() {
        return new FeedbackReporter() {};
    }

    /**
     * @return An instance of GoogleActivityController.
     */
    public GoogleActivityController createGoogleActivityController() {
        return new GoogleActivityController();
    }

    /**
     * @return An instance of {@link GSAHelper} that handles the start point of chrome's integration
     *         with GSA.
     */
    public GSAHelper createGsaHelper() {
        return new GSAHelper();
    }

    /**
     * Returns a new instance of HelpAndFeedback.
     */
    public HelpAndFeedback createHelpAndFeedback() {
        return new HelpAndFeedback();
    }

    public InstantAppsHandler createInstantAppsHandler() {
        return new InstantAppsHandler();
    }

    /**
     * @return An instance of {@link LocaleManager} that handles customized locale related logic.
     */
    public LocaleManager createLocaleManager() {
        return new LocaleManager();
    }

    /**
     * @return An instance of {@link GooglePasswordManagerUIProvider}. Will be null if one is not
     *         available.
     */
    public GooglePasswordManagerUIProvider createGooglePasswordManagerUIProvider() {
        return null;
    }

    /**
     * Returns an instance of LocationSettings to be installed as a singleton.
     */
    public LocationSettings createLocationSettings() {
        // Using an anonymous subclass as the constructor is protected.
        // This is done to deter instantiation of LocationSettings elsewhere without using the
        // getInstance() helper method.
        return new LocationSettings() {};
    }

    /**
     * @return An instance of MultiWindowUtils to be installed as a singleton.
     */
    public MultiWindowUtils createMultiWindowUtils() {
        return new MultiWindowUtils();
    }

    /**
     * @return An instance of RequestGenerator to be used for Omaha XML creation.  Will be null if
     *         a generator is unavailable.
     */
    public RequestGenerator createOmahaRequestGenerator() {
        return null;
    }

    /**
     * @return a new {@link ProcessInitializationHandler} instance.
     */
    public ProcessInitializationHandler createProcessInitializationHandler() {
        return new ProcessInitializationHandler();
    }

    /**
     * @return An instance of RevenueStats to be installed as a singleton.
     */
    public RevenueStats createRevenueStatsInstance() {
        return new RevenueStats();
    }

    /**
     * Returns a new instance of VariationsSession.
     */
    public VariationsSession createVariationsSession() {
        return new VariationsSession();
    }

    /** Returns the singleton instance of GooglePlayWebApkInstallDelegate. */
    public GooglePlayWebApkInstallDelegate getGooglePlayWebApkInstallDelegate() {
        return null;
    }

    /**
     * @return An instance of PolicyAuditor that notifies the policy system of the user's activity.
     * Only applicable when the user has a policy active, that is tracking the activity.
     */
    public PolicyAuditor getPolicyAuditor() {
        // This class has a protected constructor to prevent accidental instantiation.
        return new PolicyAuditor() {};
    }

    public void registerPolicyProviders(CombinedPolicyProvider combinedProvider) {
        combinedProvider.registerProvider(
                new AppRestrictionsProvider(ContextUtils.getApplicationContext()));
    }

    /**
     * Starts a service from {@code intent} with the expectation that it will make itself a
     * foreground service with {@link android.app.Service#startForeground(int, Notification)}.
     *
     * @param intent The {@link Intent} to fire to start the service.
     */
    public void startForegroundService(Intent intent) {
        ContextCompat.startForegroundService(ContextUtils.getApplicationContext(), intent);
    }

    /**
     * Upgrades a service from background to foreground after calling
     * {@link #startForegroundService(Intent)}.
     * @param service The service to be foreground.
     * @param id The notification id.
     * @param notification The notification attached to the foreground service.
     * @param foregroundServiceType The type of foreground service. Must be a subset of the
     *                              foreground service types defined in AndroidManifest.xml.
     *                              Use 0 if no foregroundServiceType attribute is defined.
     */
    public void startForeground(
            Service service, int id, Notification notification, int foregroundServiceType) {
        // TODO(xingliu): Add appropriate foregroundServiceType to manifest when we have new sdk.
        service.startForeground(id, notification);
    }

    /**
     * @return A callback that will be run each time an offline page is saved in the custom tabs
     * namespace.
     */
    @CalledByNative
    public Callback<CCTRequestStatus> getOfflinePagesCCTRequestDoneCallback() {
        return null;
    }

    /**
     * @return A list of whitelisted apps that are allowed to receive notification when the
     * set of offlined pages downloaded on their behalf has changed. Apps are listed by their
     * package name.
     */
    public List<String> getOfflinePagesCctWhitelist() {
        return Collections.emptyList();
    }

    /**
     * @return A list of whitelisted app package names whose completed notifications
     * we should suppress.
     */
    public List<String> getOfflinePagesSuppressNotificationPackages() {
        return Collections.emptyList();
    }

    /**
     * @return An iterator of partner bookmarks.
     */
    @Nullable
    public PartnerBookmark.BookmarkIterator getPartnerBookmarkIterator() {
        return PartnerBookmarksProviderIterator.createIfAvailable();
    }

    /**
     * @return An instance of PartnerBrowserCustomizations.Provider that provides customizations
     * specified by partners.
     */
    public PartnerBrowserCustomizations.Provider getCustomizationProvider() {
        return new PartnerBrowserCustomizations.ProviderPackage();
    }

    /**
     * @return A {@link FeedbackSourceProvider} that can provide additional {@link FeedbackSource}s
     * and {@link AsyncFeedbackSource}s to be used by a {@link FeedbackCollector}.
     */
    public FeedbackSourceProvider getAdditionalFeedbackSources() {
        return new FeedbackSourceProvider() {};
    }

    /**
     * @return a new {@link Fido2ApiHandler} instance.
     */
    public Fido2ApiHandler createFido2ApiHandler() {
        return new Fido2ApiHandler();
    }

    /**
     * @return A new {@link FeatureHighlightProvider}.
     */
    public FeatureHighlightProvider createFeatureHighlightProvider() {
        return new FeatureHighlightProvider();
    }

    /**
     * @return A new {@link DownloadCollectionBridge} instance.
     */
    public DownloadCollectionBridge getDownloadCollectionBridge() {
        return DownloadCollectionBridge.getDownloadCollectionBridge();
    }

    /**
     * @return A new {@link DigitalWellbeingClient} instance.
     */
    public DigitalWellbeingClient createDigitalWellbeingClient() {
        return new DigitalWellbeingClient();
    }

    /**
     * @param activity An activity for access to different features.
     * @return A new {@link TouchlessUiController} instance.
     */
    public TouchlessUiController createTouchlessUiController(ChromeActivity activity) {
        return null;
    }

    /**
     * Checks the Google Play services availability on the this device.
     *
     * This is a workaround for the
     * versioned API of {@link GoogleApiAvailability#isGooglePlayServicesAvailable()}. The current
     * Google Play services SDK version doesn't have this API yet.
     *
     * TODO(zqzhang): Remove this method after the SDK is updated.
     *
     * @return status code indicating whether there was an error. The possible return values are the
     * same as {@link GoogleApiAvailability#isGooglePlayServicesAvailable()}.
     */
    public int isGoogleApiAvailableWithMinApkVersion(int minApkVersion) {
        try {
            PackageInfo gmsPackageInfo =
                    ContextUtils.getApplicationContext().getPackageManager().getPackageInfo(
                            GoogleApiAvailability.GOOGLE_PLAY_SERVICES_PACKAGE, /* flags= */ 0);
            int apkVersion = gmsPackageInfo.versionCode;
            if (apkVersion >= minApkVersion) return ConnectionResult.SUCCESS;
        } catch (PackageManager.NameNotFoundException e) {
            return ConnectionResult.SERVICE_MISSING;
        }
        return ConnectionResult.SERVICE_VERSION_UPDATE_REQUIRED;
    }

    /**
     * Returns a new {@link FreIntentCreator} instance.
     */
    public FreIntentCreator createFreIntentCreator() {
        return new FreIntentCreator();
    }

    /**
     * @return true if the webAppIntent has been intercepted.
     */
    public boolean interceptWebAppIntent(Intent intent, ChromeActivity activity) {
        return false;
    }

    /**
     * @param registry The Chrome interface registry for the RenderFrameHost.
     * @param renderFrameHost The RenderFrameHost the Interface Registry is for.
     */
    public void registerChromeRenderFrameHostInterfaces(
            InterfaceRegistry registry, RenderFrameHost renderFrameHost) {
        return;
    }
}
