# Application Priority Changer

Small Windows utility (native C++ / Win32) to view running processes and change their priority classes.

Features
- Lists running processes with columns: Name, PID, Priority
- Full list refresh (default30s)
- Priority-only updates every2s (no list movement)
- Manual refresh, search filter, and "Use Selected" to copy selected process
- Apply priority (launches elevated PowerShell to change priority)
- Keeps user scroll position across automatic and manual refreshes

Requirements
- Windows7+ (uses native Win32 APIs)
- Visual Studio (recommended) with C++ workload; project is a Visual C++ project
- Builds with C++14

Build
1. Open the solution in Visual Studio: `Application Priority Changer.sln` (or open the `*.vcxproj` in the `Application Priority Changer` folder).
2. Make sure `Common Controls` are available (project already calls `InitCommonControlsEx`).
3. Build in Debug or Release.

Run
- Run the built executable. To change process priorities the app launches an elevated PowerShell instance — you must approve the UAC prompt.

Behavior / Settings
- Default full refresh interval: `30` seconds (editable in the UI).
- Priority-only updates: every `2` seconds (updates only the Priority column and does not move the list).
- While a full refresh runs the priority updater is suppressed so the list does not jump.

Notes and limitations
- Changing priorities requires elevation; the app uses `powershell.exe` (runas). Consider replacing the PowerShell approach with native `SetPriorityClass` if you want to avoid UAC.
- Priority names and numeric values are displayed (e.g. `Normal (32)`).
- The app preserves the user's scroll position across refreshes by restoring the top visible item.

Contributing
- Fork, make changes in a feature branch, and open a pull request.
- Keep changes limited to C++14 compatibility unless explicitly updating project settings.

License
- No license file included. Add a `LICENSE` file to the repository with the license you prefer (e.g. `MIT` or `Apache-2.0`).

Contact / Repo
- Repository: `https://github.com/rakinshahriar/Application-Priority-Changer` (local clone at `C:\Users\rakin\source\repos\Application Priority Changer`)

