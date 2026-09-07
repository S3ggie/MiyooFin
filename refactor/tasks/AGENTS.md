
### Direct execution for numbered refactor tasks

For numbered tasks under `refactor/tasks/`, the main Codex agent MUST perform the implementation directly.

This rule overrides the general Main Agent Role delegation instructions above for numbered refactor tasks.

- Do not spawn implementation subagents.
- Do not spawn reviewer subagents.
- Do not create SDD workspaces, ledgers, generated briefs, or review packages.
- Read the current task, inspect only the code necessary for that task, implement it directly, validate it, review the diff, commit it, and STOP.
- Use the currently selected Codex model and reasoning level for the entire task.
- If the task exposes unexpected coupling or cannot be completed safely within Allowed Files, STOP and report the problem instead of delegating or widening scope.
