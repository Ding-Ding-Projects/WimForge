# Persistent WimForge instructions

- For every task, keep user-visible application copy, documentation, release notes, and handoff summaries bilingual in English and natural Hong Kong Cantonese written in Traditional Chinese.
- Do not substitute formal written Chinese, Mandarin wording, or Simplified Chinese for the Hong Kong Cantonese half.
- Every Git commit message must be bilingual. Write the English subject first, followed by the Hong Kong Cantonese subject, separated by ` / `. If a commit body is needed, provide both languages there as well.
- Before every task is considered complete, keep `README.md` and the canonical `docs/wiki` pages synchronized with the delivered behavior.
- Refresh and visually verify every tracked image under `docs/screenshots` for every completed task. Regenerate the complete thirteen-image application gallery and both documentation-site viewport captures; never land a partial screenshot refresh.
- After each completed task, create a bilingual commit for the finished scope, push it, merge it into `main`, and verify the applicable `main`, Wiki, Pages, container, and release workflows unless the user explicitly directs a different Git workflow for that task.
- Treat these language, Git, documentation, Wiki, and screenshot rules as project-wide defaults for all future work unless the user explicitly overrides them for a specific task.

## Shared repository completion memory

- Every task that changes this repository must end with all intended task work committed and pushed.
- Review every local and remote branch, linked worktree, and stash before cleanup. Preserve useful work in commits, integrate every completed branch or worktree into the default branch, and verify each source tip is an ancestor of the pushed remote default branch.
- Never delete a branch, worktree, stash, or checkout that contains uncommitted, unmerged, or unpushed work.
- After remote proof, remove merged temporary branches, linked worktrees, their on-disk directories, stale worktree metadata, and redundant stashes.
- The final handoff target is a clean default checkout, no staged, unstaged, untracked, or stashed task work, and zero divergence from the remote default branch. Preserve and report unrelated pre-existing work instead of discarding it.
- Record significant completion and cleanup decisions in a repository-tracked handoff or memory file and push that update.
- Never force-push unless the user explicitly requests a history rewrite and the consequences have been reviewed.
