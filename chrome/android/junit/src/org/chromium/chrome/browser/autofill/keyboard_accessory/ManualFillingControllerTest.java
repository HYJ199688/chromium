// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill.keyboard_accessory;

import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.CoreMatchers.not;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.hamcrest.CoreMatchers.nullValue;
import static org.junit.Assert.assertThat;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import static org.chromium.chrome.browser.autofill.keyboard_accessory.AccessoryAction.GENERATE_PASSWORD_AUTOMATIC;
import static org.chromium.chrome.browser.autofill.keyboard_accessory.sheet_tabs.AccessorySheetTabModel.AccessorySheetDataPiece.Type.PASSWORD_INFO;
import static org.chromium.chrome.browser.autofill.keyboard_accessory.sheet_tabs.AccessorySheetTabModel.AccessorySheetDataPiece.getType;
import static org.chromium.chrome.browser.autofill.keyboard_accessory.tab_layout_component.KeyboardAccessoryTabLayoutProperties.ACTIVE_TAB;
import static org.chromium.chrome.browser.autofill.keyboard_accessory.tab_layout_component.KeyboardAccessoryTabLayoutProperties.TABS;
import static org.chromium.chrome.browser.tab.Tab.INVALID_TAB_ID;
import static org.chromium.chrome.browser.tabmodel.TabLaunchType.FROM_BROWSER_ACTIONS;
import static org.chromium.chrome.browser.tabmodel.TabSelectionType.FROM_CLOSE;
import static org.chromium.chrome.browser.tabmodel.TabSelectionType.FROM_NEW;
import static org.chromium.chrome.browser.tabmodel.TabSelectionType.FROM_USER;

import android.graphics.drawable.Drawable;
import android.support.annotation.Nullable;
import android.view.ViewGroup;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;
import org.robolectric.annotation.Config;

import org.chromium.base.UserDataHost;
import org.chromium.base.metrics.test.ShadowRecordHistogram;
import org.chromium.base.test.BaseRobolectricTestRunner;
import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.ChromeKeyboardVisibilityDelegate;
import org.chromium.chrome.browser.ChromeWindow;
import org.chromium.chrome.browser.autofill.keyboard_accessory.bar_component.KeyboardAccessoryCoordinator;
import org.chromium.chrome.browser.autofill.keyboard_accessory.bar_component.KeyboardAccessoryModernView;
import org.chromium.chrome.browser.autofill.keyboard_accessory.bar_component.KeyboardAccessoryProperties;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.KeyboardAccessoryData;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.KeyboardAccessoryData.AccessorySheetData;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.KeyboardAccessoryData.Action;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.KeyboardAccessoryData.UserInfo;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.PropertyProvider;
import org.chromium.chrome.browser.autofill.keyboard_accessory.sheet_component.AccessorySheetProperties;
import org.chromium.chrome.browser.autofill.keyboard_accessory.sheet_component.AccessorySheetView;
import org.chromium.chrome.browser.autofill.keyboard_accessory.sheet_tabs.PasswordAccessorySheetCoordinator;
import org.chromium.chrome.browser.autofill.keyboard_accessory.tab_layout_component.KeyboardAccessoryTabLayoutCoordinator;
import org.chromium.chrome.browser.autofill.keyboard_accessory.tab_layout_component.KeyboardAccessoryTabLayoutView;
import org.chromium.chrome.browser.fullscreen.ChromeFullscreenManager;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.Tab.TabHidingType;
import org.chromium.chrome.browser.tabmodel.TabModelSelector;
import org.chromium.chrome.test.util.browser.Features;
import org.chromium.chrome.test.util.browser.Features.DisableFeatures;
import org.chromium.chrome.test.util.browser.Features.EnableFeatures;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.KeyboardVisibilityDelegate;
import org.chromium.ui.display.DisplayAndroid;
import org.chromium.ui.modelutil.ListModel;
import org.chromium.ui.modelutil.ListObservable;
import org.chromium.ui.modelutil.PropertyModel;
import org.chromium.ui.test.util.modelutil.FakeViewProvider;

import java.lang.ref.WeakReference;

/**
 * Controller tests for the root controller for interactions with the manual filling UI.
 */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE, shadows = {ShadowRecordHistogram.class})
@EnableFeatures({ChromeFeatureList.PASSWORDS_KEYBOARD_ACCESSORY,
        ChromeFeatureList.AUTOFILL_KEYBOARD_ACCESSORY,
        ChromeFeatureList.AUTOFILL_MANUAL_FALLBACK_ANDROID})
@DisableFeatures({ChromeFeatureList.EXPERIMENTAL_UI})
public class ManualFillingControllerTest {
    @Mock
    private ChromeWindow mMockWindow;
    @Mock
    private ChromeActivity mMockActivity;
    private WebContents mLastMockWebContents;
    @Mock
    private ViewGroup mMockContentView;
    @Mock
    private KeyboardAccessoryModernView mMockKeyboardAccessoryView;
    @Mock
    private KeyboardAccessoryTabLayoutView mMockTabSwitcherView;
    @Mock
    private AccessorySheetView mMockViewPager;
    @Mock
    private ListObservable.ListObserver<Void> mMockTabListObserver;
    @Mock
    private ListObservable.ListObserver<Void> mMockItemListObserver;
    @Mock
    private TabModelSelector mMockTabModelSelector;
    @Mock
    private Drawable mMockIcon;
    @Mock
    private android.content.res.Resources mMockResources;
    @Mock
    private ChromeKeyboardVisibilityDelegate mMockKeyboard;

    @Rule
    public Features.JUnitProcessor mFeaturesProcessor = new Features.JUnitProcessor();

    private ManualFillingCoordinator mController = new ManualFillingCoordinator();

    private final UserDataHost mUserDataHost = new UserDataHost();

    @Before
    public void setUp() {
        ShadowRecordHistogram.reset();
        MockitoAnnotations.initMocks(this);
        KeyboardVisibilityDelegate.setInstance(mMockKeyboard);
        when(mMockWindow.getKeyboardDelegate()).thenReturn(mMockKeyboard);
        when(mMockWindow.getActivity()).thenReturn(new WeakReference<>(mMockActivity));
        when(mMockActivity.getTabModelSelector()).thenReturn(mMockTabModelSelector);
        when(mMockActivity.getActivityTabProvider()).thenReturn(mock(ActivityTabProvider.class));
        ChromeFullscreenManager fullscreenManager = new ChromeFullscreenManager(mMockActivity, 0);
        when(mMockActivity.getFullscreenManager()).thenReturn(fullscreenManager);
        when(mMockActivity.getResources()).thenReturn(mMockResources);
        when(mMockActivity.findViewById(android.R.id.content)).thenReturn(mMockContentView);
        mLastMockWebContents = mock(WebContents.class);
        when(mMockActivity.getCurrentWebContents()).then(i -> mLastMockWebContents);
        when(mMockResources.getDimensionPixelSize(anyInt())).thenReturn(48);
        PasswordAccessorySheetCoordinator.IconProvider.getInstance().setIconForTesting(mMockIcon);
        when(mMockKeyboardAccessoryView.getTabLayout()).thenReturn(mMockTabSwitcherView);
        mController.initialize(mMockWindow,
                new FakeViewProvider<>(mMockKeyboardAccessoryView),
                new FakeViewProvider<>(mMockViewPager));
    }

    @Test
    public void testCreatesValidSubComponents() {
        assertThat(mController, is(notNullValue()));
        assertThat(mController.getMediatorForTesting(), is(notNullValue()));
        assertThat(mController.getKeyboardAccessory(), is(notNullValue()));
        assertThat(mController.getMediatorForTesting().getAccessorySheet(), is(notNullValue()));
    }

    private KeyboardAccessoryTabLayoutCoordinator getTabLayout() {
        assert mController.getKeyboardAccessory() != null;
        return mController.getKeyboardAccessory().getTabLayoutForTesting();
    }

    @Test
    public void testAddingNewTabIsAddedToAccessoryAndSheet() {
        PropertyModel accessorySheetModel = mController.getMediatorForTesting()
                                                    .getAccessorySheet()
                                                    .getMediatorForTesting()
                                                    .getModelForTesting();
        accessorySheetModel.get(AccessorySheetProperties.TABS).addObserver(mMockTabListObserver);
        getTabLayout().getModelForTesting().get(TABS).addObserver(mMockTabListObserver);

        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(0));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(0));

        mController.getMediatorForTesting().getOrCreatePasswordSheet();

        verify(mMockTabListObserver)
                .onItemRangeInserted(getTabLayout().getModelForTesting().get(TABS), 0, 1);
        verify(mMockTabListObserver)
                .onItemRangeInserted(accessorySheetModel.get(AccessorySheetProperties.TABS), 0, 1);
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(1));
    }

    @Test
    public void testAddingBrowserTabsCreatesValidAccessoryState() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        ManualFillingStateCache cache = mediator.getStateCacheForTesting();
        // Emulate adding a browser tab. Expect the model to have another entry.
        Tab firstTab = addTab(mediator, 1111, null);
        ManualFillingState firstState = cache.getStateFor(firstTab);
        assertThat(firstState, notNullValue());

        // Emulate adding a second browser tab. Expect the model to have another entry.
        Tab secondTab = addTab(mediator, 2222, firstTab);
        ManualFillingState secondState = cache.getStateFor(secondTab);
        assertThat(secondState, notNullValue());

        assertThat(firstState, not(equalTo(secondState)));
    }

    @Test
    public void testPasswordItemsPersistAfterSwitchingBrowserTabs() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        PropertyProvider<AccessorySheetData> firstTabProvider = new PropertyProvider<>();
        PropertyProvider<AccessorySheetData> secondTabProvider = new PropertyProvider<>();

        // Simulate opening a new tab which automatically triggers the registration:
        Tab firstTab = addTab(mediator, 1111, null);
        mController.registerPasswordProvider(firstTabProvider);
        firstTabProvider.notifyObservers(createPasswordData("FirstPassword"));

        assertThat(getFirstPassword(mediator), is("FirstPassword"));

        // Simulate creating a second tab:
        Tab secondTab = addTab(mediator, 2222, firstTab);
        mController.registerPasswordProvider(secondTabProvider);
        secondTabProvider.notifyObservers(createPasswordData("SecondPassword"));
        assertThat(getFirstPassword(mediator), is("SecondPassword"));

        // Simulate switching back to the first tab:
        switchTab(mediator, /*from=*/secondTab, /*to=*/firstTab);
        assertThat(getFirstPassword(mediator), is("FirstPassword"));

        // And back to the second:
        switchTab(mediator, /*from=*/firstTab, /*to=*/secondTab);
        assertThat(getFirstPassword(mediator), is("SecondPassword"));
    }

    @Test
    public void testKeyboardAccessoryActionsPersistAfterSwitchingBrowserTabs() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        PropertyProvider<Action[]> firstTabProvider =
                new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);
        PropertyProvider<Action[]> secondTabProvider =
                new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);
        ListModel<KeyboardAccessoryProperties.BarItem> keyboardActions =
                mediator.getKeyboardAccessory().getMediatorForTesting().getModelForTesting().get(
                        KeyboardAccessoryProperties.BAR_ITEMS);
        keyboardActions.addObserver(mMockItemListObserver);

        // Simulate opening a new tab which automatically triggers the registration:
        Tab firstTab = addTab(mediator, 1111, null);
        mController.registerActionProvider(firstTabProvider);
        firstTabProvider.notifyObservers(new Action[] {
                new Action("Generate Password", GENERATE_PASSWORD_AUTOMATIC, p -> {})});
        mMockItemListObserver.onItemRangeInserted(keyboardActions, 0, 2);
        assertThat(getFirstKeyboardActionTitle(), is("Generate Password"));

        // Simulate creating a second tab:
        Tab secondTab = addTab(mediator, 2222, firstTab);
        mController.registerActionProvider(secondTabProvider);
        secondTabProvider.notifyObservers(new Action[0]);
        mMockItemListObserver.onItemRangeRemoved(keyboardActions, 0, 1);
        assertThat(keyboardActions.size(), is(1)); // tab switcher is only item on this browser tab.

        // Simulate switching back to the first tab:
        switchTab(mediator, /*from=*/secondTab, /*to=*/firstTab);
        mMockItemListObserver.onItemRangeInserted(keyboardActions, 0, 1);
        assertThat(getFirstKeyboardActionTitle(), is("Generate Password"));

        // And back to the second:
        switchTab(mediator, /*from=*/firstTab, /*to=*/secondTab);
        mMockItemListObserver.onItemRangeRemoved(keyboardActions, 0, 1);
        assertThat(keyboardActions.size(), is(1)); // tab switcher is only item on this browser tab.
    }

    @Test
    public void testPasswordTabRestoredWhenSwitchingBrowserTabs() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        PropertyModel accessorySheetModel =
                mediator.getAccessorySheet().getMediatorForTesting().getModelForTesting();

        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(0));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(0));

        // Create a new tab with a passwords tab:
        Tab firstTab = addTab(mediator, 1111, null);
        mController.registerPasswordProvider(new PropertyProvider<>());
        // There should be a tab in accessory and sheet:
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(1));

        // Simulate creating a second tab without any tabs:
        Tab secondTab = addTab(mediator, 2222, firstTab);
        // There should be no tab in accessory and sheet:
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(0));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(0));

        // Simulate switching back to the first tab:
        switchTab(mediator, /*from=*/secondTab, /*to=*/firstTab);
        // There should be a tab in accessory and sheet:
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(1));

        // And back to the second:
        switchTab(mediator, /*from=*/firstTab, /*to=*/secondTab);
        // Still no tab in accessory and sheet:
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(0));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(0));
    }

    @Test
    public void testPasswordTabRestoredWhenClosingTabIsUndone() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        ManualFillingStateCache cache = mediator.getStateCacheForTesting();
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(0));

        // Create a new tab with a passwords tab:
        Tab tab = addTab(mediator, 1111, null);
        mController.registerPasswordProvider(new PropertyProvider<>());
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));

        // Simulate closing the tab (uncommitted):
        mediator.getTabModelObserverForTesting().willCloseTab(tab, false);
        mediator.getTabObserverForTesting().onHidden(tab, TabHidingType.CHANGED_TABS);
        cache.getStateFor(mLastMockWebContents).getWebContentsObserverForTesting().wasHidden();
        mLastMockWebContents = null;

        // Simulate undo closing the tab and selecting it:
        mediator.getTabModelObserverForTesting().tabClosureUndone(tab);
        switchTab(mediator, null, tab);
        // There should still be a tab in the accessory:
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));

        // Simulate closing the tab and committing to it (i.e. wait out undo message):
        closeTab(mediator, tab, null);
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(0));
    }

    @Test
    public void testTreatNeverProvidedActionsAsEmptyActionList() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        PropertyModel keyboardAccessoryModel =
                mediator.getKeyboardAccessory().getMediatorForTesting().getModelForTesting();

        // Open a tab.
        Tab tab = addTab(mediator, 1111, null);
        // Add an action provider that never provided actions.
        mController.registerActionProvider(new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC));
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(1));

        // Create a new tab with an action:
        Tab secondTab = addTab(mediator, 1111, tab);
        PropertyProvider<Action[]> provider = new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);
        mController.registerActionProvider(provider);
        provider.notifyObservers(new Action[] {
                new Action("Test Action", GENERATE_PASSWORD_AUTOMATIC, (action) -> {})});
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(2));

        switchTab(mediator, secondTab, tab);
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(1));
    }

    @Test
    public void testUpdatesInactiveAccessory() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        PropertyModel keyboardAccessoryModel =
                mediator.getKeyboardAccessory().getMediatorForTesting().getModelForTesting();

        // Open a tab.
        Tab tab = addTab(mediator, 1111, null);
        // Add an action provider that hasn't provided actions yet.
        PropertyProvider<Action[]> delayedProvider =
                new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);
        mController.registerActionProvider(delayedProvider);
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(1));

        // Create and switch to a new tab:
        Tab secondTab = addTab(mediator, 2222, tab);
        PropertyProvider<Action[]> provider = new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);
        mController.registerActionProvider(provider);

        // And provide data to the active browser tab.
        provider.notifyObservers(new Action[] {
                new Action("Test Action", GENERATE_PASSWORD_AUTOMATIC, (action) -> {})});
        // Now, have the delayed provider provide data for the backgrounded browser tab.
        delayedProvider.notifyObservers(
                new Action[] {new Action("Delayed", GENERATE_PASSWORD_AUTOMATIC, (action) -> {})});

        // The current tab should not be influenced by the delayed provider.
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(2));
        assertThat(getFirstKeyboardActionTitle(), is("Test Action"));

        // Switching tabs back should only show the action that was received in the background.
        switchTab(mediator, secondTab, tab);
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(2));
        assertThat(getFirstKeyboardActionTitle(), is("Delayed"));
    }

    @Test
    public void testDestroyingTabCleansModelForThisTab() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        ManualFillingStateCache cache = mediator.getStateCacheForTesting();
        PropertyModel keyboardAccessoryModel =
                mediator.getKeyboardAccessory().getMediatorForTesting().getModelForTesting();
        PropertyModel accessorySheetModel = mController.getMediatorForTesting()
                                                    .getAccessorySheet()
                                                    .getMediatorForTesting()
                                                    .getModelForTesting();

        PropertyProvider<AccessorySheetData> firstTabProvider = new PropertyProvider<>();
        PropertyProvider<Action[]> firstActionProvider =
                new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);
        PropertyProvider<AccessorySheetData> secondTabProvider = new PropertyProvider<>();
        PropertyProvider<Action[]> secondActionProvider =
                new PropertyProvider<>(GENERATE_PASSWORD_AUTOMATIC);

        // Simulate opening a new tab:
        Tab firstTab = addTab(mediator, 1111, null);
        mController.registerPasswordProvider(firstTabProvider);
        mController.registerActionProvider(firstActionProvider);
        firstTabProvider.notifyObservers(createPasswordData("FirstPassword"));
        firstActionProvider.notifyObservers(new Action[] {
                new Action("2BDestroyed", GENERATE_PASSWORD_AUTOMATIC, (action) -> {})});

        // Create and switch to a new tab: (because destruction shouldn't rely on tab to be active)
        addTab(mediator, 2222, firstTab);
        mController.registerPasswordProvider(secondTabProvider);
        mController.registerActionProvider(secondActionProvider);
        secondTabProvider.notifyObservers(createPasswordData("SecondPassword"));
        secondActionProvider.notifyObservers(
                new Action[] {new Action("2BKept", GENERATE_PASSWORD_AUTOMATIC, (action) -> {})});

        // The current tab should be valid.
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(1));
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(2));
        assertThat(getFirstKeyboardActionTitle(), is("2BKept"));

        // Request destruction of the first Tab:
        mediator.getTabObserverForTesting().onDestroyed(firstTab);

        // The current tab should not be influenced by the destruction.
        assertThat(getTabLayout().getModelForTesting().get(TABS).size(), is(1));
        assertThat(accessorySheetModel.get(AccessorySheetProperties.TABS).size(), is(1));
        assertThat(keyboardAccessoryModel.get(KeyboardAccessoryProperties.BAR_ITEMS).size(), is(2));
        assertThat(getFirstKeyboardActionTitle(), is("2BKept"));

        // The other tabs data should be gone.
        ManualFillingState oldState = cache.getStateFor(firstTab);
        if (oldState == null)
            return; // Having no state is fine - it would be completely destroyed then.

        assertThat(oldState.getActionsProvider(), nullValue());

        if (oldState.getPasswordAccessorySheet() == null)
            return; // Having no password sheet is fine - it would be completely destroyed then.
        assertThat(
                oldState.getPasswordAccessorySheet().getSheetDataPiecesForTesting().size(), is(0));
    }

    @Test
    public void testResumingWithoutActiveTabClearsStateAndHidesKeyboard() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();

        // Show the accessory bar to make sure it would be dismissed.
        getTabLayout().getTabSwitchingDelegate().addTab(
                new KeyboardAccessoryData.Tab("Passwords", null, null, 0, 0, null));
        mediator.getKeyboardAccessory().requestShowing();
        assertThat(mediator.getKeyboardAccessory().isShown(), is(true));

        // No active tab was added - still we request a resume. This should just clear everything.
        mController.onResume();

        assertThat(mediator.getKeyboardAccessory().isShown(), is(false));
        // |getOrCreatePasswordSheet| creates a sheet if the state allows it. Here, it shouldn't.
        assertThat(
                mediator.getOrCreatePasswordSheet().getSheetDataPiecesForTesting().size(), is(0));
    }

    @Test
    public void testDisplaysAccessoryOnlyWhenSpaceIsSufficient() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();
        Tab tab = addTab(mediator, 1234, null);
        when(mMockKeyboard.isSoftKeyboardShowing(eq(mMockActivity), any())).thenReturn(true);

        // Show the accessory bar for the default dimensions (300x80@2.f).
        mediator.getOrCreatePasswordSheet();
        mediator.showWhenKeyboardIsVisible();
        assertThat(mediator.getKeyboardAccessory().isShown(), is(true));

        // Use a width that is too small (e.g. on tiny phones).
        simulateOrientationChange(mediator, 2.0f, 170, 80);
        assertThat(mediator.getKeyboardAccessory().isShown(), is(false));

        // Use a height that is too small but with a valid width (e.g. rotated to landscape).
        simulateOrientationChange(mediator, 2.0f, 600, 20);
        assertThat(mediator.getKeyboardAccessory().isShown(), is(false));

        // Use valid dimension at another density.
        simulateOrientationChange(mediator, 1.5f, 180, 80);
        assertThat(mediator.getKeyboardAccessory().isShown(), is(true));

        // Now that the accessory is shown, the content area is already smaller due to the bar.
        setContentAreaDimensions(3.f, 180, (80 - /* bar height = */ 48));
        mediator.onLayoutChange(mMockContentView, 0, 0, 540, 96, 0, 0, 270, 120);
        assertThat(mediator.getKeyboardAccessory().isShown(), is(true));
    }

    @Test
    public void testClosingTabDoesntAffectUnitializedComponents() {
        ManualFillingMediator mediator = mController.getMediatorForTesting();

        // A leftover tab is closed before the filling component could pick up the active tab.
        closeTab(mediator, mock(Tab.class), null);

        // Without any tab, there should be no state that would allow creating a sheet.
        assertThat(mediator.getOrCreatePasswordSheet(), is(nullValue()));
    }

    @Test
    public void testIsFillingViewShownReturnsTargetValueAheadOfComponentUpdate() {
        // After initialization with one tab, the accessory sheet is closed.
        KeyboardAccessoryCoordinator accessory =
                mController.getMediatorForTesting().getKeyboardAccessory();
        addTab(mController.getMediatorForTesting(), 1234, null);
        mController.getMediatorForTesting().getOrCreatePasswordSheet();
        accessory.requestShowing();
        assertThat(mController.isFillingViewShown(null), is(false));

        // As soon as active tab and keyboard change, |isFillingViewShown| returns the expected
        // state - even if the sheet component wasn't updated yet.
        getTabLayout().getModelForTesting().set(ACTIVE_TAB, 0);
        when(mMockKeyboard.isSoftKeyboardShowing(eq(mMockActivity), any())).thenReturn(false);
        assertThat(mController.isFillingViewShown(null), is(true));

        // The layout change impacts the component, but not the coordinator method.
        mController.getMediatorForTesting().onLayoutChange(null, 0, 0, 0, 0, 0, 0, 0, 0);
        assertThat(mController.isFillingViewShown(null), is(true));
    }

    /**
     * Creates a tab and calls the observer events as if it was just created and switched to.
     * @param mediator The {@link ManualFillingMediator} whose observers should be triggered.
     * @param id The id of the new browser tab.
     * @param lastTab A previous mocked {@link Tab} to be hidden. Needs |getId()|. May be null.
     * @return Returns a mock of the newly added {@link Tab}. Provides |getId()|.
     */
    private Tab addTab(ManualFillingMediator mediator, int id, @Nullable Tab lastTab) {
        ManualFillingStateCache cache = mediator.getStateCacheForTesting();
        int lastId = INVALID_TAB_ID;
        if (lastTab != null) {
            lastId = lastTab.getId();
            mediator.getTabObserverForTesting().onHidden(lastTab, TabHidingType.CHANGED_TABS);
            cache.getStateFor(mLastMockWebContents).getWebContentsObserverForTesting().wasHidden();
        }
        Tab tab = mock(Tab.class);
        when(tab.getId()).thenReturn(id);
        when(tab.getUserDataHost()).thenReturn(mUserDataHost);
        mLastMockWebContents = mock(WebContents.class);
        when(tab.getWebContents()).thenReturn(mLastMockWebContents);
        cache.getStateFor(tab).getWebContentsObserverForTesting().wasShown();
        when(tab.getContentView()).thenReturn(mMockContentView);
        when(mMockActivity.getActivityTabProvider().getActivityTab()).thenReturn(tab);
        when(mMockTabModelSelector.getCurrentTab()).thenReturn(tab);
        mediator.getTabModelObserverForTesting().didAddTab(tab, FROM_BROWSER_ACTIONS);
        mediator.getTabObserverForTesting().onShown(tab, FROM_NEW);
        mediator.getTabModelObserverForTesting().didSelectTab(tab, FROM_NEW, lastId);
        setContentAreaDimensions(2.f, 300, 80);
        return tab;
    }

    /**
     * Simulates switching to a different tab by calling observer events on the given |mediator|.
     * @param mediator The mediator providing the observer instances.
     * @param from The mocked {@link Tab} to be switched from. Needs |getId()|. May be null.
     * @param to The mocked {@link Tab} to be switched to. Needs |getId()|.
     */
    private void switchTab(ManualFillingMediator mediator, @Nullable Tab from, Tab to) {
        ManualFillingStateCache cache = mediator.getStateCacheForTesting();
        int lastId = INVALID_TAB_ID;
        if (from != null) {
            lastId = from.getId();
            mediator.getTabObserverForTesting().onHidden(from, TabHidingType.CHANGED_TABS);
            cache.getStateFor(mLastMockWebContents).getWebContentsObserverForTesting().wasHidden();
        }
        mLastMockWebContents = to.getWebContents();
        cache.getStateFor(to).getWebContentsObserverForTesting().wasShown();
        when(mMockTabModelSelector.getCurrentTab()).thenReturn(to);
        mediator.getTabModelObserverForTesting().didSelectTab(to, FROM_USER, lastId);
        mediator.getTabObserverForTesting().onShown(to, FROM_USER);
    }

    /**
     * Simulates destroying the given tab by calling observer events on the given |mediator|.
     * @param mediator The mediator providing the observer instances.
     * @param tabToBeClosed The mocked {@link Tab} to be closed. Needs |getId()|.
     * @param next A mocked {@link Tab} to be switched to. Needs |getId()|. May be null.
     */
    private void closeTab(ManualFillingMediator mediator, Tab tabToBeClosed, @Nullable Tab next) {
        ManualFillingStateCache cache = mediator.getStateCacheForTesting();
        mediator.getTabModelObserverForTesting().willCloseTab(tabToBeClosed, false);
        mediator.getTabObserverForTesting().onHidden(tabToBeClosed, TabHidingType.CHANGED_TABS);
        cache.getStateFor(mLastMockWebContents).getWebContentsObserverForTesting().wasHidden();
        mLastMockWebContents = null;
        if (next != null) {
            when(mMockTabModelSelector.getCurrentTab()).thenReturn(next);
            cache.getStateFor(next).getWebContentsObserverForTesting().wasShown();
            mLastMockWebContents = next.getWebContents();
            mediator.getTabModelObserverForTesting().didSelectTab(
                    next, FROM_CLOSE, tabToBeClosed.getId());
        }
        mediator.getTabModelObserverForTesting().tabClosureCommitted(tabToBeClosed);
        mediator.getTabObserverForTesting().onDestroyed(tabToBeClosed);
    }

    private void setContentAreaDimensions(float density, int widthDp, int heightDp) {
        DisplayAndroid mockDisplay = mock(DisplayAndroid.class);
        when(mockDisplay.getDipScale()).thenReturn(density);
        when(mMockWindow.getDisplay()).thenReturn(mockDisplay);
        when(mLastMockWebContents.getHeight()).thenReturn(heightDp);
        when(mLastMockWebContents.getWidth()).thenReturn(widthDp);
    }

    /**
     * This function initializes mocks and then calls the given mediator events in the order in
     * which a rotation call would trigger them.
     * It mains sets the {@link WebContents} size and calls |onLayoutChange| with the new bounds.
     * @param mediator The mediator to be called.
     * @param density The logical screen density (e.g. 1.f).
     * @param width The new {@link WebContents} width in dp.
     * @param height The new {@link WebContents} height in dp.
     */
    private void simulateOrientationChange(
            ManualFillingMediator mediator, float density, int width, int height) {
        int oldHeight = mLastMockWebContents.getHeight();
        int oldWidth = mLastMockWebContents.getWidth();
        int newHeight = (int) (density * height);
        int newWidth = (int) (density * width);
        setContentAreaDimensions(density, width, height);
        // A rotation always closes the keyboard for a brief period before reopening it.
        when(mMockKeyboard.isSoftKeyboardShowing(eq(mMockActivity), any())).thenReturn(false);
        mediator.onLayoutChange(
                mMockContentView, 0, 0, newWidth, newHeight, 0, 0, oldWidth, oldHeight);
        when(mMockKeyboard.isSoftKeyboardShowing(eq(mMockActivity), any())).thenReturn(true);
        mediator.onLayoutChange(
                mMockContentView, 0, 0, newWidth, newHeight, 0, 0, oldWidth, oldHeight);
    }

    private AccessorySheetData createPasswordData(String text) {
        AccessorySheetData sheetData =
                new AccessorySheetData(FallbackSheetType.PASSWORD, "Passwords");
        UserInfo userInfo = new UserInfo(null);
        userInfo.addField(new UserInfo.Field("(No username)", "No username", false, null));
        userInfo.addField(new UserInfo.Field(text, "Password", true, null));
        sheetData.getUserInfoList().add(userInfo);
        return sheetData;
    }

    private String getFirstPassword(ManualFillingMediator mediator) {
        PasswordAccessorySheetCoordinator passwordSheet = mediator.getOrCreatePasswordSheet();
        assert passwordSheet != null;
        assert passwordSheet.getSheetDataPiecesForTesting() != null;
        assert passwordSheet.getSheetDataPiecesForTesting().size() > 0;
        assert getType(passwordSheet.getSheetDataPiecesForTesting().get(0)) == PASSWORD_INFO;
        UserInfo info =
                (UserInfo) passwordSheet.getSheetDataPiecesForTesting().get(0).getDataPiece();
        assert info.getFields().size() > 1;
        return info.getFields().get(1).getDisplayText();
    }

    private String getFirstKeyboardActionTitle() {
        int firstNonTabSwitcherAction = 1;
        if (ChromeFeatureList.isEnabled(ChromeFeatureList.AUTOFILL_KEYBOARD_ACCESSORY)) {
            firstNonTabSwitcherAction = 0;
        }
        return mController.getMediatorForTesting()
                .getKeyboardAccessory()
                .getMediatorForTesting()
                .getModelForTesting()
                .get(KeyboardAccessoryProperties.BAR_ITEMS)
                .get(firstNonTabSwitcherAction)
                .getAction()
                .getCaption();
    }
}
