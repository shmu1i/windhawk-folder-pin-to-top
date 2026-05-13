# Pin Folders to Top

A [Windhawk](https://windhawk.net) mod for Windows 11 that pins specific files
or folders to the top of their parent in File Explorer.

Right-click any file or folder and pick **"📌 Pin to top"**. Right-click it
again to **"📌 Unpin from top"**. Pinned items rise to the top of views
sorted or grouped by Date Modified (e.g. the default Downloads view).

Designed for Windows 11's new XAML File Explorer (and works in classic Explorer
too).

## Install

1. Install [Windhawk](https://windhawk.net).
2. In Windhawk, click **Create new mod**.
3. Paste the contents of [`folder-pin-to-top.wh.cpp`](folder-pin-to-top.wh.cpp).
4. Compile (Ctrl+B), then save and enable.

Pin state is stored in the registry under
`HKCU\Software\WindhawkFolderPin\Pins` and survives mod reloads.

## How it works

The mod rewrites the timestamps a pinned item appears to have, so the shell
treats the pinned item as though it were modified about a week in the future.
With Date Modified sorted/grouped descending (the default for Downloads),
this lands the pinned item at the top.

Three hook surfaces:

1. **`NtQueryDirectoryFile` / `NtQueryDirectoryFileEx`** in `ntdll.dll` —
   rewrites timestamps in the kernel-level directory enumeration, beating any
   shell-level cache. This is what actually moves pinned items in the view.
2. **`TrackPopupMenuEx`** in `user32.dll` — injects the Pin/Unpin item into
   Explorer's classic context menu using the `TPM_RETURNCMD` trick to
   intercept the click without registering a shell extension.
3. **`CFSFolder::GetDetailsEx`** in `windows.storage.dll` — belt-and-suspenders
   for shell callers that re-fetch timestamps after enumeration.

The Pin/Unpin handler updates the registry and fires
`SHChangeNotify(SHCNE_UPDATEDIR, …)` for the affected parent, so the view
refreshes automatically.

## Visible side effect

The Date Modified column shows a date ~7 days in the future for pinned items.
This is intrinsic to the mechanism — there's no way to defeat date grouping
without the date itself reflecting the change.

## Limitations

- Sorting/grouping by Name, Size, or Type — pinned items appear in their
  natural alphabetical/size/type position, not at the top.
- Ascending sort by Date — future dates are "largest", so ascending puts
  pinned items at the bottom. Most people sort newest-first.
- Virtual locations like Home, Quick Access, Gallery, and libraries are not
  affected — only filesystem folders backed by `CFSFolder`.
- Items must be direct children of the folder you're viewing.
- On Windows 11 with the default modern (collapsed) context menu, the
  Pin/Unpin item appears under **"Show more options"**. To make it visible at
  the top level, enable the classic context menu via the
  [Classic context menu on Windows 11](https://windhawk.net/mods/explorer-context-menu-classic)
  mod or the equivalent registry tweak.

## License

[MIT](LICENSE).
