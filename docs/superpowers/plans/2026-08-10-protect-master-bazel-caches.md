# Protect Default-Branch Bazel Caches Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop pull-request Bazel jobs from writing GitHub Actions caches while allowing Windows and macOS PR jobs to restore corresponding default-branch build caches.

**Architecture:** `pr.yml` makes cache saving explicitly read-only and shares ordinary-build cache namespaces with `release.yml`. Sonar retains its distinct read-only namespace. The release workflow remains a cache writer through setup-bazel's default behavior.

**Tech Stack:** GitHub Actions YAML, bazel-contrib/setup-bazel 0.19.0, Bazel.

## Global Constraints

- Do not change workflow triggers, build commands, or cache paths.
- `pr.yml` must use literal `cache-save: false`; it has no non-PR trigger.
- Ordinary-build identifiers must be `fastecu-build-${{ runner.os }}` in both workflows.
- SonarCloud must use `fastecu-sonar-${{ runner.os }}` and remain separate.
- Leave cache saving enabled by default in `release.yml`.

---

### Task 1: Configure read-only PR caches and shared default-branch build keys

**Files:**
- Modify: `.github/workflows/pr.yml:51-55,93-97`
- Modify: `.github/workflows/release.yml:106-110`
- Test: GitHub Actions YAML parse and repository text assertions

**Interfaces:**
- Consumes: `bazel-contrib/setup-bazel` inputs `disk-cache` and `cache-save`.
- Produces: Restore-only PR cache consumers and default-branch ordinary-build cache writers keyed by platform.

- [ ] **Step 1: Run the failing workflow-policy assertion**

```bash
ruby -e 'require "yaml"; pr = File.read(".github/workflows/pr.yml"); release = File.read(".github/workflows/release.yml"); abort "missing PR cache-save policy" unless pr.scan(/cache-save: false/).size == 2; abort "PR ordinary cache key is not shared" unless pr.include?("disk-cache: fastecu-build-${{ runner.os }}"); abort "release ordinary cache key is not shared" unless release.include?("disk-cache: fastecu-build-${{ runner.os }}"); abort "Sonar cache key is not separated" unless pr.include?("disk-cache: fastecu-sonar-${{ runner.os }}")'
```

Expected: FAIL because no PR cache save policy exists and the cache names are workflow-derived.

- [ ] **Step 2: Update the SonarCloud setup-bazel configuration**

In `.github/workflows/pr.yml`, replace `disk-cache: ${{ github.workflow }}-sonar` with:

```yaml
disk-cache: fastecu-sonar-${{ runner.os }}
cache-save: false
```

- [ ] **Step 3: Update ordinary build-cache configurations**

In `.github/workflows/pr.yml`, replace `disk-cache: ${{ github.workflow }}` with:

```yaml
disk-cache: fastecu-build-${{ runner.os }}
cache-save: false
```

In `.github/workflows/release.yml`, replace `disk-cache: ${{ github.workflow }}-${{ runner.os }}` with:

```yaml
disk-cache: fastecu-build-${{ runner.os }}
```

- [ ] **Step 4: Run policy assertions and formatting checks**

```bash
ruby -e 'require "yaml"; pr = File.read(".github/workflows/pr.yml"); release = File.read(".github/workflows/release.yml"); abort "missing PR cache-save policy" unless pr.scan(/cache-save: false/).size == 2; abort "PR ordinary cache key is not shared" unless pr.include?("disk-cache: fastecu-build-${{ runner.os }}"); abort "release ordinary cache key is not shared" unless release.include?("disk-cache: fastecu-build-${{ runner.os }}"); abort "Sonar cache key is not separated" unless pr.include?("disk-cache: fastecu-sonar-${{ runner.os }}")'
git diff --check
```

Expected: both commands exit with status 0.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/pr.yml .github/workflows/release.yml docs/superpowers/plans/2026-08-10-protect-master-bazel-caches.md
git commit -m "ci: prevent PR Bazel cache eviction"
```
