import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const KIOSK_IFACE_XML = `
<node>
  <interface name="net.okidia.Kiosk">
    <method name="SetScreenCastUnsafe">
      <arg type="b" direction="in" name="enabled"/>
    </method>
    <method name="ForceUninhibit"/>
    <method name="SetLockdown">
      <arg type="b" direction="in" name="locked"/>
    </method>
    <method name="SetLockOverlay">
      <arg type="b" direction="in" name="locked"/>
      <arg type="s" direction="in" name="title"/>
      <arg type="s" direction="in" name="message"/>
    </method>
    <method name="ListWindows">
      <arg type="s" direction="out" name="json"/>
    </method>
    <method name="CloseWindow">
      <arg type="t" direction="in" name="wid"/>
      <arg type="u" direction="out" name="pid"/>
    </method>
    <method name="CloseAllWindows">
      <arg type="u" direction="out" name="closedCount"/>
    </method>
  </interface>
</node>`;

export default class OkidiaKioskExtension extends Extension {
    enable() {
        this._prevUnsafe = false;
        this._unsafeTimeoutId = 0;
        this._lockdownActive = false;
        this._lockOverlayActive = false;
        this._lockTitle = 'Ekranınız Kilitlendi';
        this._lockMessage = 'Lütfen öğretmeninizi dinleyin.';
        this._lockCurtainActor = null;
        this._lockCurtainMonitorSig = '';
        this._lockTitleLabels = [];
        this._lockMessageLabels = [];
        this._origOverviewToggle = null;
        this._origOverviewShow = null;
        this._remoteControllerSigId = 0;
        this._sessionModeSigId = 0;
        this._blockedSignalIds = [];
        this._origInhibitRemoteAccess = null;
        this._origUninhibitRemoteAccess = null;
        this._uninhibitedOnce = false;
        this._savedHasWindows = null;
        this._hidScreenShield = false;
        this._poppedShieldModal = false;

        this._blockUnsafeModeNotificationHandlers();
        this._uninhibitRemoteAccess();

        if (Main.sessionMode?.connect) {
            this._sessionModeSigId = Main.sessionMode.connect('updated', () => {
                this._uninhibitRemoteAccess();
            });
        }

        this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(KIOSK_IFACE_XML, this);
        this._dbusImpl.export(Gio.DBus.session, '/net/okidia/Kiosk');
        this._dbusNameId = Gio.bus_own_name(
            Gio.BusType.SESSION,
            'net.okidia.Kiosk',
            Gio.BusNameOwnerFlags.REPLACE,
            null, null, null
        );

        try {
            const rac = global.backend?.get_remote_access_controller?.();
            if (rac) {
                this._remoteControllerSigId = rac.connect('new-handle', (_ctrl, handle) => {
                    this._suppressHandle(handle);
                    this._hideScreenShareControls();
                    GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                        this._hideScreenShareControls();
                        return GLib.SOURCE_REMOVE;
                    });
                });
            }
        } catch (_e) {
            // Sürüm farklarında sessiz kal
        }

        this._timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 400, () => {
            this._uninhibitRemoteAccess();
            this._hideScreenShareControls();
            this._purgeUnsafeModeNotifications();
            this._enforceLockdownState();
            return GLib.SOURCE_CONTINUE;
        });
        this._hideScreenShareControls();
        this._purgeUnsafeModeNotifications();
    }

    disable() {
        this.SetLockdown(false);

        if (this._sessionModeSigId && Main.sessionMode?.disconnect) {
            try {
                Main.sessionMode.disconnect(this._sessionModeSigId);
            } catch (_e) {}
            this._sessionModeSigId = 0;
        }

        this._restoreRemoteAccessInhibition();

        if (this._remoteControllerSigId) {
            try {
                const rac = global.backend?.get_remote_access_controller?.();
                if (rac) rac.disconnect(this._remoteControllerSigId);
            } catch (_e) {}
            this._remoteControllerSigId = 0;
        }
        if (this._unsafeTimeoutId) {
            GLib.source_remove(this._unsafeTimeoutId);
            this._unsafeTimeoutId = 0;
        }
        if (global?.context) {
            this._setUnsafeModeSilent(false);
        }
        if (this._dbusImpl) {
            this._dbusImpl.unexport();
            this._dbusImpl = null;
        }
        if (this._dbusNameId) {
            Gio.bus_unown_name(this._dbusNameId);
            this._dbusNameId = 0;
        }
        if (this._timerId) {
            GLib.source_remove(this._timerId);
            this._timerId = 0;
        }
    }

    _uninhibitRemoteAccess() {
        try {
            const rac = global.backend?.get_remote_access_controller?.();
            const wasDisallowed = Main.sessionMode && !Main.sessionMode.allowScreencast;

            if (Main.sessionMode) {
                Main.sessionMode.allowScreencast = true;
            }

            if (rac) {
                if (!this._origInhibitRemoteAccess && rac.inhibit_remote_access) {
                    this._origInhibitRemoteAccess = rac.inhibit_remote_access.bind(rac);
                }
                if (!this._origUninhibitRemoteAccess && rac.uninhibit_remote_access) {
                    this._origUninhibitRemoteAccess = rac.uninhibit_remote_access.bind(rac);
                }

                // GDM ve unlock-dialog modlarında GNOME Shell başlangıçta allowScreencast=false
                // olduğu için rac.inhibit_remote_access() çağırmıştır (inhibit_count = 1).
                // Bu sayacı bir kez düşürüp ardından inhibit/uninhibit fonksiyonlarını no-op yapıyoruz.
                if (wasDisallowed && !this._uninhibitedOnce && this._origUninhibitRemoteAccess) {
                    this._uninhibitedOnce = true;
                    try {
                        this._origUninhibitRemoteAccess();
                    } catch (_e) {}
                }

                rac.inhibit_remote_access = () => {};
                rac.uninhibit_remote_access = () => {};
            }
        } catch (_e) {}
    }

    _restoreRemoteAccessInhibition() {
        try {
            const rac = global.backend?.get_remote_access_controller?.();
            if (rac) {
                if (this._origInhibitRemoteAccess) {
                    rac.inhibit_remote_access = this._origInhibitRemoteAccess;
                    this._origInhibitRemoteAccess = null;
                }
                if (this._origUninhibitRemoteAccess) {
                    rac.uninhibit_remote_access = this._origUninhibitRemoteAccess;
                    this._origUninhibitRemoteAccess = null;
                }
            }
            this._uninhibitedOnce = false;
        } catch (_e) {}
    }

    _blockUnsafeModeNotificationHandlers() {
        if (!global?.context) return;
        try {
            const criteria = {signalId: 'notify', detail: 'unsafe-mode'};
            for (let i = 0; i < 8; i++) {
                const hid = GObject.signal_handler_find(global.context, criteria);
                if (!hid || hid <= 0) break;
                GObject.signal_handler_disconnect(global.context, hid);
            }
        } catch (_e) {}
    }

    _purgeUnsafeModeNotifications() {
        try {
            const sysSrc = MessageTray.getSystemSource?.();
            if (sysSrc) {
                const sysNotifs = sysSrc.notifications ?? sysSrc._notifications ?? [];
                for (const n of Array.from(sysNotifs)) {
                    n?.destroy?.();
                }
            }
            const mt = Main.messageTray;
            if (!mt) return;
            const sources = mt.getSources?.() ?? mt._sources ?? [];
            const list = Array.isArray(sources) ? sources : Array.from(sources);
            for (const src of list) {
                if (!src) continue;
                const isSys = (src === sysSrc) ||
                              (MessageTray.SystemNotificationSource && src instanceof MessageTray.SystemNotificationSource) ||
                              (src.constructor?.name === 'SystemNotificationSource');
                const notifs = src.notifications ?? src._notifications ?? [];
                for (const n of Array.from(notifs)) {
                    const hasUndo = (n?.actions?.length > 0) || (n?._actions?.length > 0);
                    if (isSys || hasUndo) {
                        n.destroy?.();
                    }
                }
                if (isSys && src !== sysSrc) {
                    src.destroy?.();
                }
            }
        } catch (_e) {}
    }

    _setUnsafeModeSilent(val) {
        if (!global?.context) return;
        this._blockUnsafeModeNotificationHandlers();

        const mt = Main.messageTray;
        const origMtAdd = mt?.add;
        const origMtAddSource = mt?._addSource;
        const srcProto = MessageTray.Source?.prototype;
        const origAddNotif = srcProto?.addNotification;
        const origShowNotif = srcProto?.showNotification;
        const sysSrc = MessageTray.getSystemSource?.();
        const origSysAddNotif = sysSrc?.addNotification;
        const origSysShowNotif = sysSrc?.showNotification;

        try {
            if (mt && origMtAdd) mt.add = () => {};
            if (mt && origMtAddSource) mt._addSource = () => {};
            if (srcProto && origAddNotif) srcProto.addNotification = () => {};
            if (srcProto && origShowNotif) srcProto.showNotification = () => {};
            if (sysSrc && origSysAddNotif) sysSrc.addNotification = () => {};
            if (sysSrc && origSysShowNotif) sysSrc.showNotification = () => {};

            global.context.unsafe_mode = Boolean(val);
        } catch (_e) {
        } finally {
            if (mt && origMtAdd) mt.add = origMtAdd;
            if (mt && origMtAddSource) mt._addSource = origMtAddSource;
            if (srcProto && origAddNotif) srcProto.addNotification = origAddNotif;
            if (srcProto && origShowNotif) srcProto.showNotification = origShowNotif;
            if (sysSrc && origSysAddNotif) sysSrc.addNotification = origSysAddNotif;
            if (sysSrc && origSysShowNotif) sysSrc.showNotification = origSysShowNotif;
        }

        this._purgeUnsafeModeNotifications();
    }

    _suppressHandle(handle) {
        if (!handle) return;
        try {
            handle.stop = () => {};
        } catch (_e) {}
    }

    ForceUninhibit() {
        this._uninhibitRemoteAccess();
        try {
            if (this._origUninhibitRemoteAccess) {
                this._origUninhibitRemoteAccess();
            }
        } catch (_e) {}
    }

    SetScreenCastUnsafe(enabled) {
        if (this._unsafeTimeoutId) {
            GLib.source_remove(this._unsafeTimeoutId);
            this._unsafeTimeoutId = 0;
        }
        this._uninhibitRemoteAccess();
        this._hideScreenShareControls();
        if (!global?.context) return;

        if (enabled) {
            this._prevUnsafe = global.context.unsafe_mode;
            this._setUnsafeModeSilent(true);
            this._unsafeTimeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 10, () => {
                this._unsafeTimeoutId = 0;
                this._setUnsafeModeSilent(this._prevUnsafe);
                return GLib.SOURCE_REMOVE;
            });
        } else {
            this._setUnsafeModeSilent(this._prevUnsafe);
        }
    }

    SetLockOverlay(locked, title, message) {
        this._lockOverlayActive = Boolean(locked);
        if (title && String(title).trim().length > 0) {
            this._lockTitle = String(title);
        } else if (!this._lockTitle) {
            this._lockTitle = 'Ekranınız Kilitlendi';
        }
        if (message && String(message).trim().length > 0) {
            this._lockMessage = String(message);
        } else if (!this._lockMessage) {
            this._lockMessage = 'Lütfen öğretmeninizi dinleyin.';
        }
        this.SetLockdown(Boolean(locked));
        this._syncNativeLockCurtain();
    }

    SetLockdown(locked) {
        this._lockdownActive = Boolean(locked);
        if (!this._lockdownActive) {
            this._lockOverlayActive = false;
        }
        try {
            if (this._lockdownActive) {
                if (Main.overview) {
                    if (!this._origOverviewToggle) {
                        this._origOverviewToggle = Main.overview.toggle?.bind(Main.overview);
                    }
                    if (!this._origOverviewShow) {
                        this._origOverviewShow = Main.overview.show?.bind(Main.overview);
                    }
                    Main.overview.hide?.();
                    Main.overview.toggle = () => {};
                    Main.overview.show = () => {};
                }
                this._syncNativeLockCurtain();
                this._enforceLockdownState();
            } else {
                if (Main.overview) {
                    if (this._origOverviewToggle) {
                        Main.overview.toggle = this._origOverviewToggle;
                        this._origOverviewToggle = null;
                    }
                    if (this._origOverviewShow) {
                        Main.overview.show = this._origOverviewShow;
                        this._origOverviewShow = null;
                    }
                }
                this._destroyNativeLockCurtain();
                this._restoreScreenShieldLayer();
            }
        } catch (_e) {}
    }

    _createVerticalBox(props = {}) {
        try {
            return new St.BoxLayout({
                ...props,
                orientation: Clutter.Orientation.VERTICAL,
            });
        } catch (_e) {
            return new St.BoxLayout({
                ...props,
                vertical: true,
            });
        }
    }

    _destroyNativeLockCurtain() {
        this._lockTitleLabels = [];
        this._lockMessageLabels = [];
        this._lockCurtainMonitorSig = '';
        if (this._lockCurtainActor) {
            try {
                const parent = this._lockCurtainActor.get_parent?.();
                if (parent) {
                    parent.remove_child(this._lockCurtainActor);
                }
                this._lockCurtainActor.destroy?.();
            } catch (_e) {}
            this._lockCurtainActor = null;
        }
    }

    _syncNativeLockCurtain() {
        try {
            const shouldShow = this._lockOverlayActive ||
                (this._lockdownActive && (Main.sessionMode?.isGreeter || Main.sessionMode?.isLocked));
            if (!shouldShow) {
                this._destroyNativeLockCurtain();
                return;
            }

            const stageW = global.stage?.width || 1920;
            const stageH = global.stage?.height || 1080;
            const rawMonitors = Main.layoutManager?.monitors;
            const monitors = (Array.isArray(rawMonitors) && rawMonitors.length > 0)
                ? rawMonitors
                : [{x: 0, y: 0, width: stageW, height: stageH}];
            const monitorSig = monitors.map(m => `${m.x},${m.y},${m.width},${m.height}`).join(';') + `|${stageW}x${stageH}`;

            if (this._lockCurtainActor && this._lockCurtainMonitorSig !== monitorSig) {
                this._destroyNativeLockCurtain();
            }

            if (!this._lockCurtainActor) {
                const root = new St.Widget({
                    name: 'okidia-lock-curtain',
                    reactive: true,
                    x: 0,
                    y: 0,
                    width: stageW,
                    height: stageH,
                    style: 'background-color: #000000;',
                });

                this._lockTitleLabels = [];
                this._lockMessageLabels = [];

                for (const mon of monitors) {
                    const monBin = new St.Bin({
                        reactive: true,
                        x: mon.x,
                        y: mon.y,
                        width: mon.width,
                        height: mon.height,
                        x_expand: true,
                        y_expand: true,
                        x_align: Clutter.ActorAlign.CENTER,
                        y_align: Clutter.ActorAlign.CENTER,
                        style: 'background-color: #000000;',
                    });

                    const box = this._createVerticalBox({
                        x_align: Clutter.ActorAlign.CENTER,
                        y_align: Clutter.ActorAlign.CENTER,
                        style: 'spacing: 16px; padding: 48px;',
                    });

                    const icon = new St.Icon({
                        icon_name: 'changes-prevent-symbolic',
                        icon_size: 64,
                        x_align: Clutter.ActorAlign.CENTER,
                        style: 'color: #ffffff; margin-bottom: 8px;',
                    });

                    const titleLbl = new St.Label({
                        text: this._lockTitle || 'Ekranınız Kilitlendi',
                        x_align: Clutter.ActorAlign.CENTER,
                        style: 'color: #ffffff; font-size: 28px; font-weight: bold; text-align: center;',
                    });

                    const msgLbl = new St.Label({
                        text: this._lockMessage || 'Lütfen öğretmeninizi dinleyin.',
                        x_align: Clutter.ActorAlign.CENTER,
                        style: 'color: #aaaaaa; font-size: 16px; text-align: center; padding-top: 4px;',
                    });
                    if (msgLbl.clutter_text) {
                        msgLbl.clutter_text.line_wrap = true;
                    }

                    box.add_child(icon);
                    box.add_child(titleLbl);
                    box.add_child(msgLbl);
                    monBin.set_child(box);
                    root.add_child(monBin);

                    this._lockTitleLabels.push(titleLbl);
                    this._lockMessageLabels.push(msgLbl);
                }

                const parentGroup = Main.layoutManager?.uiGroup ?? global.stage;
                if (parentGroup) {
                    parentGroup.add_child(root);
                    parentGroup.set_child_above_sibling?.(root, null);
                }
                this._lockCurtainActor = root;
                this._lockCurtainMonitorSig = monitorSig;
            } else {
                const titleText = this._lockTitle || 'Ekranınız Kilitlendi';
                const msgText = this._lockMessage || 'Lütfen öğretmeninizi dinleyin.';
                for (const lbl of this._lockTitleLabels) {
                    if (lbl && lbl.text !== titleText) lbl.text = titleText;
                }
                for (const lbl of this._lockMessageLabels) {
                    if (lbl && lbl.text !== msgText) lbl.text = msgText;
                }
                this._lockCurtainActor.visible = true;
                this._lockCurtainActor.opacity = 255;
                this._lockCurtainActor.show?.();
                const parentGroup = this._lockCurtainActor.get_parent?.();
                if (parentGroup) {
                    parentGroup.set_child_above_sibling?.(this._lockCurtainActor, null);
                }
            }
        } catch (_e) {}
    }

    _restoreScreenShieldLayer() {
        try {
            if (this._savedHasWindows !== null && Main.sessionMode) {
                Main.sessionMode.hasWindows = this._savedHasWindows;
                this._savedHasWindows = null;
            }
            if (this._hidScreenShield && Main.layoutManager?.screenShieldGroup) {
                Main.layoutManager.screenShieldGroup.visible = true;
                Main.layoutManager.screenShieldGroup.show?.();
                this._hidScreenShield = false;
            }
            if (this._poppedShieldModal && Main.screenShield) {
                try {
                    Main.screenShield._becomeModal?.();
                } catch (_e) {}
                this._poppedShieldModal = false;
            }
            if (Main.layoutManager?._updateVisibility) {
                Main.layoutManager._updateVisibility();
            }
        } catch (_e) {}
    }

    _enforceLockdownState() {
        try {
            this._syncNativeLockCurtain();

            const okidiaWindows = [];
            const actors = global.get_window_actors?.() ?? [];
            for (const actor of actors) {
                const win = actor.get_meta_window?.();
                if (!win) continue;
                const wmClass = (win.get_wm_class?.() ?? '').toLowerCase();
                const title = (win.get_title?.() ?? '').toLowerCase();
                if (wmClass.includes('okidia-tray') || wmClass.includes('okidia') || title.includes('okidia')) {
                    okidiaWindows.push({actor, win});
                }
            }

            const hasOkidiaWindow = okidiaWindows.length > 0;
            if (!this._lockdownActive && !hasOkidiaWindow) {
                if (this._hidScreenShield || this._savedHasWindows !== null || this._poppedShieldModal) {
                    this._restoreScreenShieldLayer();
                }
                return;
            }

            if (Main.overview?.visible) {
                Main.overview.hide?.();
            }

            for (const {actor, win} of okidiaWindows) {
                actor.visible = true;
                actor.opacity = 255;
                actor.show?.();
                if (win.minimized) {
                    win.unminimize?.();
                }
                win.make_above?.();
                win.raise?.();
                win.activate?.(global.get_current_time?.() ?? 0);
            }

            // Yerel Clutter kilit perdesi (_lockCurtainActor) her zaman uiGroup'un en üstünde kalsın
            if (this._lockCurtainActor) {
                const parentGroup = this._lockCurtainActor.get_parent?.();
                if (parentGroup) {
                    parentGroup.set_child_above_sibling?.(this._lockCurtainActor, null);
                }
            }
        } catch (_e) {}
    }

    ListWindows() {
        const result = [];
        try {
            const actors = global.get_window_actors?.() ?? [];
            for (const actor of actors) {
                const win = actor.get_meta_window?.();
                if (!win) continue;
                if (win.is_override_redirect?.() || win.is_skip_taskbar?.()) continue;

                const wType = win.get_window_type?.();
                if (wType !== undefined &&
                    wType !== Meta.WindowType.NORMAL &&
                    wType !== Meta.WindowType.DIALOG &&
                    wType !== Meta.WindowType.MODAL_DIALOG) {
                    continue;
                }

                const title = (win.get_title?.() ?? '').trim();
                if (!title) continue;

                const wmClass = (win.get_wm_class?.() ?? '').trim();
                const wmClassLow = wmClass.toLowerCase();
                if (wmClassLow.includes('okidia-tray') ||
                    wmClassLow.includes('okidia-agent') ||
                    wmClassLow.includes('okidia-master') ||
                    wmClassLow.includes('okidia-broker') ||
                    wmClassLow === 'gnome-shell' ||
                    wmClassLow === 'mutter') {
                    continue;
                }

                const pid = win.get_pid?.() ?? 0;
                const wid = win.get_stable_sequence?.() ?? win.get_id?.() ?? 0;
                const rect = win.get_frame_rect?.() ?? {x: 0, y: 0, width: 0, height: 0};

                result.push({
                    wid: Number(wid),
                    pid: Number(pid),
                    title: title,
                    wm_class: wmClass,
                    x: Number(rect.x ?? 0),
                    y: Number(rect.y ?? 0),
                    w: Number(rect.width ?? 0),
                    h: Number(rect.height ?? 0)
                });
            }
        } catch (_e) {}
        return JSON.stringify(result);
    }

    CloseWindow(wid) {
        let targetPid = 0;
        try {
            const actors = global.get_window_actors?.() ?? [];
            for (const actor of actors) {
                const win = actor.get_meta_window?.();
                if (!win) continue;
                const curWid = win.get_stable_sequence?.() ?? win.get_id?.() ?? 0;
                if (Number(curWid) === Number(wid)) {
                    targetPid = Number(win.get_pid?.() ?? 0);
                    win.delete?.(global.get_current_time?.() ?? 0);
                    win.kill?.();
                    break;
                }
            }
        } catch (_e) {}
        return targetPid;
    }

    CloseAllWindows() {
        let count = 0;
        try {
            const actors = global.get_window_actors?.() ?? [];
            for (const actor of actors) {
                const win = actor.get_meta_window?.();
                if (!win) continue;
                if (win.is_override_redirect?.() || win.is_skip_taskbar?.()) continue;

                const wType = win.get_window_type?.();
                if (wType !== undefined &&
                    wType !== Meta.WindowType.NORMAL &&
                    wType !== Meta.WindowType.DIALOG &&
                    wType !== Meta.WindowType.MODAL_DIALOG) {
                    continue;
                }

                const wmClassLow = (win.get_wm_class?.() ?? '').toLowerCase();
                if (wmClassLow.includes('okidia') ||
                    wmClassLow === 'gnome-shell' ||
                    wmClassLow === 'mutter') {
                    continue;
                }

                win.delete?.(global.get_current_time?.() ?? 0);
                win.kill?.();
                count++;
            }
        } catch (_e) {}
        return count;
    }

    _neutralizeWidget(w) {
        if (!w) return;
        try {
            w._sync = () => {
                w.visible = false;
                w.opacity = 0;
                w.reactive = false;
                w.hide?.();
            };
            w._updateVisibility = w._sync;
            w.visible = false;
            w.opacity = 0;
            w.reactive = false;
            w.hide?.();
        } catch (_e) {}
    }

    _hideScreenShareControls() {
        try {
            const qs = Main.panel?.statusArea?.quickSettings;
            if (qs) {
                const ra = qs._remoteAccess;
                if (ra) {
                    this._neutralizeWidget(ra);
                    if (ra._indicator) {
                        this._neutralizeWidget(ra._indicator);
                    }
                    if (ra._recordingItem) {
                        this._neutralizeWidget(ra._recordingItem);
                    }
                    if (ra._sharingItem) {
                        this._neutralizeWidget(ra._sharingItem);
                    }
                    if (ra.quickSettingsItems) {
                        for (const item of ra.quickSettingsItems) {
                            this._neutralizeWidget(item);
                        }
                    }
                    if (ra._handles) {
                        for (const h of ra._handles) {
                            this._suppressHandle(h);
                        }
                    }
                }
            }
            const statusArea = Main.panel?.statusArea;
            if (statusArea) {
                for (const key of ['screenSharing', 'screenRecording', 'remoteAccess']) {
                    if (statusArea[key]) {
                        this._neutralizeWidget(statusArea[key]);
                    }
                }
            }
        } catch (_e) {
            // GNOME Shell sürüm farklarında sessiz kal
        }
    }
}
