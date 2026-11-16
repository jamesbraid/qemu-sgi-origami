# Releasing

A release is an annotated tag named `sgi-origami-<QEMU VERSION>-<n>`, where
`<QEMU VERSION>` is the tree's `VERSION` file and `<n>` counts releases on
that line. Pushing the tag runs `.github/workflows/release.yml`, which builds
the archives (see `packaging/README.md`) and publishes them. The tag message
is the release body, word for word. Nothing is appended to it, so the tag is
the durable copy: write it for a reader who has not followed the branch.

Draft the message in a file in your worktree and tag with
`git tag -a -F <file> sgi-origami-<version>-<n>`. Tag dates are authoritative,
so the message carries none.

## Message format

```
sgi-origami X.Y.Z-N

<one sentence: what this release is about>

<optional highlights: one to three short paragraphs, big changes only>

Breaking:
- <component>: <what changed for the operator>. Migration: <the step>.

Features:
- <component>: <what an operator can now do, or what now behaves differently>.
- Experimental: <component>: <behaviour>. The interface may change before it is stable.

Changed:
- <component>: <a visible change, including what the archive contains or requires>.

Fixes:
- <component>: <what was wrong, and what now happens>.
```

- The title line is `sgi-origami X.Y.Z-N`.
- Sections come in that order. Leave out an empty one.
- Breaking comes first, and each item names its migration step.
- A bullet is `Component: sentence`, kept short. Name the subsystem an operator
  would, such as `CD-ROM:`, `Origin 3900:` or `qtest:`. Put detail in the manual
  page or document and link it from the bullet.
- `Experimental:` leads the bullet for an interface that may change
  incompatibly.
- Archive contents and requirements go under Changed. A packaging bug goes
  under Fixes. There is no Packaging section.
- There is no Known issues section. Fix the issue or hold the release; a limit
  that must stay belongs in the manual page.
- Leave out CI, refactors and test changes unless they change behaviour,
  performance or what is supported. No marketing voice.
- The repository is public: no internal paths, codenames or issue numbers.
