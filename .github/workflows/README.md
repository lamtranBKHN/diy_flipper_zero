# CI/CD Workflows

- `diy-build.yml` / `diy-lint.yml`: Active DIY-specific CI for this fork.
- `build.yml` / `lint.yml` / `release.yml` / `pr-cleanup.yml` / `webhook.yml`: Upstream Momentum FW CI (kept for merge compatibility). These reference Momentum infra secrets (`INDEXER_URL`, `INDEXER_TOKEN`, `REPO_DISPATCH_TOKEN`) not present in this fork.
