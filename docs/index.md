---
title: WimForge
description: A reviewable, Git-backed studio for building customized Windows images.
hide:
  - navigation
  - toc
---

<section class="wf-hero" markdown>
<div class="wf-hero__copy" markdown>

<span class="wf-eyebrow">OPEN SOURCE · WINDOWS · MATERIAL DESIGN</span>

# Forge Windows images with every change reviewable.

WimForge brings offline image servicing, unattended setup, package selection,
Group Policy, WinForge-family staging, and recovery history into one focused
desktop studio. Your source stays untouched by default, your plan stays
inspectable, and project changes stay Git-backed.

[Get started](wiki/Getting-Started.md){ .md-button .md-button--primary }
[Explore the app](wiki/Application-Tour.md){ .md-button }

<div class="wf-hero__meta" markdown>
**C++20 + Qt 6.8** · **Windows x64** · **MIT licensed**
</div>
</div>

<div class="wf-hero__visual" markdown>
![WimForge Overview showing the project dashboard, navigation, build flow, safety rails, and job status](screenshots/overview.png){ loading=eager }
</div>
</section>

<div class="wf-value-grid" markdown>
<article class="wf-value-card" markdown>
### Review before elevation
See executable paths, argument tokens, dependencies, destructive flags, and
localized descriptions before any servicing plan runs.
</article>

<article class="wf-value-card" markdown>
### History without erasure
Every successful configuration change becomes a project commit. Selective undo
adds a compensating event instead of rewriting the past.
</article>

<article class="wf-value-card" markdown>
### Portable by design
Complete `.wimforge` saves carry project and notification repositories,
including the Git history needed for recovery and inspection.
</article>
</div>

## One workspace, one reviewable flow

<div class="wf-flow" markdown>
1. **Choose a source**  
   Inspect legally obtained ISO media, WIM, ESD, or split SWM sources.
2. **Shape the image**  
   Configure servicing, policies, unattended setup, packages, and typed bridge actions.
3. **Review the graph**  
   Validate paths, hashes, dependencies, checkpoints, and output destinations.
4. **Run and recover**  
   Execute with journaled progress, then export or save the complete project history.
</div>

!!! warning "Test the result before production"
    WimForge orchestrates Windows tools; it does not remove the need for
    licensing review, exact-image validation, backups, and a disposable virtual
    machine test. Read [Safety and Recovery](wiki/Safety-and-Recovery.md) before
    servicing a production image.

## Built for the work around the image

<div class="grid cards" markdown>

-   :material-image-edit-outline:{ .lg .middle } **Image servicing**

    ---

    Drivers, updates, features, capabilities, Appx provisioning, registry
    changes, output formats, and ISO publication in a dependency-aware plan.

    [:octicons-arrow-right-24: Image servicing](wiki/Image-Servicing.md)

-   :material-package-variant-closed:{ .lg .middle } **Package Studio**

    ---

    Portable profiles, provider-specific validation, dependency ordering,
    offline payloads, trust checks, and resumable first-logon installation.

    [:octicons-arrow-right-24: Package Studio](wiki/Package-Studio.md)

-   :material-history:{ .lg .middle } **History Time Machine**

    ---

    Append-only events, guarded selective undo, redo-of-undo, restore points,
    bookmarks, Git inspection, and A/B diff workflows.

    [:octicons-arrow-right-24: History](wiki/History-Time-Machine.md)

-   :material-console:{ .lg .middle } **Complete CLI**

    ---

    Deterministic JSON, response files, stable exit codes, dry runs, project
    editing, histories, bundles, studios, and bridge staging.

    [:octicons-arrow-right-24: CLI reference](wiki/CLI.md)

</div>

## See the desktop

<div class="wf-shot-grid" markdown>
<figure markdown>
![Package Studio showing the Full AI Development profile and validated package cards](screenshots/package-studio.png)
<figcaption>Package Studio keeps providers, identities, and selection state visible.</figcaption>
</figure>

<figure markdown>
![History Time Machine showing append-only actions, branch filtering, undo controls, and the live comparison pane](screenshots/history.png)
<figcaption>History actions compensate or restore; they never pretend external side effects vanished.</figcaption>
</figure>
</div>

[Open the complete screenshot gallery](gallery.md){ .md-button }
[Browse the full wiki](wiki.md){ .md-button }
[Search the wiki](wiki-search.md){ .md-button }

## Choose your next stop

<div class="wf-next" markdown>
- New to WimForge? Start with [Getting Started](wiki/Getting-Started.md).
- Preparing an image? Read [Projects and Sources](wiki/Projects-and-Sources.md) and [Review and Run](wiki/Review-and-Run.md).
- Automating builds? Use the [CLI](wiki/CLI.md) and [detailed command reference](cli.md).
- Auditing the design? Read [Architecture and Data Layout](wiki/Architecture-and-Data-Layout.md).
- Ready to contribute? See [Building and Releases](wiki/Building-and-Releases.md) and [Contributing](wiki/Contributing.md).
</div>
