# QuietGuard 0.2 native dashboard

## Visual direction

The supplied reference screenshot was inspected directly before implementation. Its useful principles are a quiet `#f5f5f5` sidebar, `#ececec` workspace, white cards, thin header dividers, pale-blue selected navigation and icon tiles, and `#007aff` primary actions. QuietGuard retains its own power icon, identity, functions, and safety language. No reference branding or unrelated features are reproduced.

The native window has three functional pages:

- **Overview:** monitoring state and four resource cards; two columns when there is room, one on a narrow effective work area
- **Monitoring rules:** original metric checkboxes, thresholds, duration, sampling interval, and warning duration
- **Startup & protection:** original startup mode selector and session-protection checkbox, with the service/login tradeoffs visible

The footer stays outside the scrolling content viewport. Start/Pause, Save, Cancel, and Help remain reachable on all pages. A native text footer shows notices and the shutdown countdown on every page. Navigation does not apply configuration or refill draft edits. The initial 1060×688-DIP client target is clamped to the actual work area; the resizable window has a work-area-aware 640×300-DIP outer minimum. There is no horizontal scroll style or horizontal-scroll handler. At small effective sizes, only the content viewport scrolls vertically; the sidebar, page title, notices, and actions remain fixed.

## Typography and accessibility

The application explicitly prefers Microsoft YaHei UI and enumerates its availability, falling back to the Windows message font (then Segoe UI) if unavailable. Body and secondary text are 14 DIP, card labels 15 DIP, and page/state titles 22 DIP. Windows handles glyph fallback. Fonts are set at creation and DPI changes, not resent on every layout or status update.

Checkboxes remain native `BS_AUTOCHECKBOX`; numeric/decimal edits, static labels, combo box, and scrollbar retain native behavior. Sidebar and action buttons remain native `BUTTON` windows with owner drawing, keyboard activation, accelerator text, tab stops, disabled rendering, and visible focus rectangles. The current page has a native static heading; the selected navigation button's accessible text includes “当前” while the visual text remains concise. Parenthesized mnemonic hints follow Windows keyboard-cue visibility, while full native captions remain accessible. Enter activates only a deliberately focused enabled action/navigation button, with auto-repeat suppressed. Enter in a numeric edit never starts, pauses, or applies monitoring; the combo box retains its native selection behavior. Control labels keep metric names and mnemonics. Tab/mnemonic focus inside a scrolled page is brought into the content viewport. The content window uses `WS_EX_CONTROLPARENT`; it clips its children so controls cannot cover the fixed footer. High contrast uses Windows window/text/highlight colors instead of the reference palette. Native screen-reader and keyboard behavior still requires Windows verification.

## Paint and idle-work boundaries

- One reusable memory DC/bitmap serves serialized shell/content paints. Its dimensions match the visible top-level client and it is allocated only during painting
- At most 4,194,304 pixels (16 MiB at 32 bits/pixel) are buffered. An oversized window or allocation failure uses direct clipped painting; buffering is not promised for those cases
- The bitmap is recreated only when dimensions change and is freed when hidden, minimized, or destroyed. Minimized windows are explicitly excluded because `IsWindowVisible` alone is insufficient
- Each paint clips drawing to the dirty rectangle and blits only that rectangle. Both parent and content use `WS_CLIPCHILDREN`; background erase is handled by the paint path
- Child moves are collected into separate same-parent `DeferWindowPos` batches. Unchanged geometry/visibility is skipped. A batch failure replays the entire affected batch with `SetWindowPos`; it cannot silently lose earlier moves
- A dedicated child vertical scrollbar cannot change the top-level client rectangle. Its range/page/position are updated only when changed. Scrolling invalidates the content, rather than the complete window. This removes the previous `WM_SIZE → SetScrollInfo → layout` feedback path and per-control immediate `MoveWindow(TRUE)` repainting
- Status text is changed only when its formatted value changes. No full-window invalidation occurs for ordinary metric/status updates. Tray text is modified only when changed
- Visibility and resize events reconcile a UI-only timer; they do not restart the monitor's sampling timer. Paused local UI has no periodic UI timer. Hidden/minimized UI has none. Show/restore refreshes text after the actual visibility transition, including second-instance activation. Visible warning/ready counts down at 1 Hz, and visible service status polls at 2 seconds, as before
- There is no WebView, managed GUI runtime, animation timer, `WS_EX_COMPOSITED`, broad `WM_SETREDRAW`, configuration write on navigation, or monitoring/core/provider/service refactor

The explicit Cancel action remains available while an administrator operation or cancellation confirmation is pending. Existing close guards, generation/epoch checks, safe dry-run behavior, service confirmation semantics, and post-submit warnings remain in place.

## Validation and limits

`tests/ui_layout_tests.c` runs the actual portable geometry function, including effective DIP sizes for 1366×768 at 100%, 150%, and 200%, plus widths around column/sidebar transitions. It checks containment, footer visibility, minimum action/control widths, no card/header overlap, deterministic layout, and scroll clamping. This proves geometry arithmetic, not actual font rasterization, Win32 paint timing, native accessibility, or flicker behavior.

Strict Windows cross-compilation and the existing core/safety/mock suites are required for the final tree. The 0.2 UI has not been run on Windows in this cloud-only refresh. Historical paused-state readings do not validate active-monitor performance,
visible CPU, flicker or GDI lifetime. Native rendering and accessibility testing
remain necessary; see [validation limits](VALIDATION.md).
