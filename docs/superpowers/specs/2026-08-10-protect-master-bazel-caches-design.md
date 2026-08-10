# Protect Default-Branch Bazel Caches

## Goal

Prevent pull-request jobs from consuming GitHub Actions cache storage while
preserving cache restoration for compatible default-branch builds.

## Design

`pr.yml` will configure both `bazel-contrib/setup-bazel` steps with
`cache-save: false`. Pull-request jobs therefore restore any matching cache
but never create or update a cache entry.

The ordinary build disk cache will use the stable name
`fastecu-build-${{ runner.os }}` in both `pr.yml` and `release.yml`. This
allows Windows and macOS pull-request jobs to restore the corresponding cache
saved by the default-branch release workflow. The SonarCloud job retains its
separate `fastecu-sonar-${{ runner.os }}` cache name because its coverage
configuration is intentionally distinct. It is restore-only and has no
default-branch seed job in this change.

`release.yml` continues to use setup-bazel's default `cache-save: true`.
Linux and SonarCloud caches are not seeded by the release workflow; they can
run cold until a dedicated default-branch CI seed workflow is added.

## Validation

Use a YAML parse check and repository text assertions to verify the two PR
setup-bazel invocations explicitly disable saving and that release and PR
ordinary-build disk-cache names match.
