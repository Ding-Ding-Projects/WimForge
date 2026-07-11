(() => {
  "use strict";

  const header = document.querySelector("[data-header]");
  const navToggle = document.querySelector("[data-nav-toggle]");
  const nav = document.querySelector("[data-nav]");
  const toast = document.querySelector("[data-toast]");
  let toastTimer;

  const showToast = (message) => {
    if (!toast) return;
    toast.textContent = message;
    toast.classList.add("is-visible");
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => toast.classList.remove("is-visible"), 2400);
  };

  const updateHeader = () => header?.classList.toggle("is-scrolled", window.scrollY > 10);
  updateHeader();
  window.addEventListener("scroll", updateHeader, { passive: true });

  const setNavigationOpen = (open, restoreFocus = false) => {
    nav?.classList.toggle("is-open", open);
    navToggle?.setAttribute("aria-expanded", String(open));
    const accessibleLabel = navToggle?.querySelector(".sr-only");
    if (accessibleLabel) accessibleLabel.textContent = open ? "Close navigation" : "Open navigation";
    if (restoreFocus) navToggle?.focus({ preventScroll: true });
  };

  navToggle?.addEventListener("click", () => {
    setNavigationOpen(!nav?.classList.contains("is-open"));
  });

  nav?.querySelectorAll("a").forEach((link) => {
    link.addEventListener("click", () => {
      setNavigationOpen(false);
    });
  });

  document.addEventListener("click", (event) => {
    if (!nav?.classList.contains("is-open")) return;
    if (!nav.contains(event.target) && !navToggle?.contains(event.target)) {
      setNavigationOpen(false);
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && nav?.classList.contains("is-open")) {
      setNavigationOpen(false, true);
    }
  });

  const settleImage = (image) => {
    const shell = image.closest(".media-shell");
    if (!shell) return;
    if (image.naturalWidth > 0) {
      shell.classList.add("is-image-ready");
      shell.classList.remove("is-image-missing");
    } else {
      shell.classList.remove("is-image-ready");
      shell.classList.add("is-image-missing");
    }
  };

  const bindImage = (image) => {
    image.addEventListener("load", () => settleImage(image));
    image.addEventListener("error", () => settleImage(image));
    if (image.complete) settleImage(image);
  };
  document.querySelectorAll("[data-image]").forEach(bindImage);

  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const reveals = document.querySelectorAll(".reveal");
  if (reduceMotion || !("IntersectionObserver" in window)) {
    reveals.forEach((item) => item.classList.add("is-visible"));
  } else {
    const revealObserver = new IntersectionObserver((entries, observer) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add("is-visible");
        observer.unobserve(entry.target);
      });
    }, { rootMargin: "0px 0px -7%", threshold: 0.06 });
    reveals.forEach((item) => revealObserver.observe(item));
  }

  const sections = [...document.querySelectorAll("main section[id]")];
  const navLinks = [...(nav?.querySelectorAll("a[href^='#']") ?? [])];
  if ("IntersectionObserver" in window) {
    const sectionObserver = new IntersectionObserver((entries) => {
      const visible = entries.filter((entry) => entry.isIntersecting).sort((a, b) => b.intersectionRatio - a.intersectionRatio)[0];
      if (!visible) return;
      navLinks.forEach((link) => {
        const current = link.getAttribute("href") === `#${visible.target.id}`;
        if (current) link.setAttribute("aria-current", "true");
        else link.removeAttribute("aria-current");
      });
    }, { rootMargin: "-25% 0px -65%", threshold: [0, 0.1, 0.5] });
    sections.forEach((section) => sectionObserver.observe(section));
  }

  const legacyCopyText = (text) => {
    const activeElement = document.activeElement;
    const selection = document.getSelection();
    const savedRanges = selection ? [...Array(selection.rangeCount)].map((_, index) => selection.getRangeAt(index)) : [];
    const textarea = document.createElement("textarea");
    textarea.value = text;
    textarea.readOnly = true;
    textarea.tabIndex = -1;
    textarea.setAttribute("aria-hidden", "true");
    Object.assign(textarea.style, {
      position: "fixed",
      top: "0",
      left: "-9999px",
      width: "1px",
      height: "1px",
      opacity: "0"
    });
    document.body.append(textarea);

    let copied = false;
    try {
      textarea.focus({ preventScroll: true });
      textarea.select();
      textarea.setSelectionRange(0, textarea.value.length);
      copied = document.execCommand("copy");
    } catch {
      copied = false;
    } finally {
      textarea.remove();
      if (selection) {
        selection.removeAllRanges();
        savedRanges.forEach((range) => selection.addRange(range));
      }
      if (activeElement instanceof HTMLElement) activeElement.focus({ preventScroll: true });
    }
    return copied;
  };

  const copyText = async (text) => {
    if (navigator.clipboard?.writeText) {
      try {
        await navigator.clipboard.writeText(text);
        return true;
      } catch {
        // Local previews and locked-down browsers can deny the modern API.
      }
    }
    return legacyCopyText(text);
  };

  document.querySelectorAll("[data-copy-target]").forEach((button) => {
    button.addEventListener("click", async () => {
      const target = document.getElementById(button.dataset.copyTarget);
      const originalLabel = button.textContent;
      if (!target) {
        button.textContent = "Copy failed";
        showToast("The copy target is unavailable.");
        window.setTimeout(() => { button.textContent = originalLabel; }, 2200);
        return;
      }
      const text = target.innerText.replace(/^PS>\s?/gm, "").trim();
      const copyLabel = button.dataset.copyLabel || "CLI examples";
      button.disabled = true;
      const copied = await copyText(text);
      button.disabled = false;
      if (copied) {
        button.textContent = "Copied";
        showToast(`${copyLabel} copied to the clipboard.`);
      } else {
        button.textContent = "Copy failed";
        showToast(`Could not copy ${copyLabel.toLowerCase()}. Select the command and copy it manually.`);
      }
      window.setTimeout(() => { button.textContent = originalLabel; }, 2200);
    });
  });

  const gallery = [
    { src: "assets/site/hero-forge.webp", title: "WimForge workspace", alt: "WimForge desktop overview", caption: "A connected Windows image project with reviewed operations and visible safety state." },
    { src: "assets/site/vm-lab.webp", title: "Virtual Machine Lab", alt: "Virtual Machine Lab", caption: "Discover VMware and VirtualBox, manage machines and snapshots, and record validation evidence." },
    { src: "assets/site/image-servicing.webp", title: "Image Servicing", alt: "Image servicing plan", caption: "Compose typed DISM operations and inspect their dependency graph before execution." },
    { src: "assets/site/unattended.webp", title: "Unattended Studio", alt: "Unattended Studio", caption: "Build portable automation profiles and round-trip Windows answer-file XML." },
    { src: "assets/site/package-studio.webp", title: "Package Studio", alt: "Package Studio", caption: "Stage trusted online and offline installers in a resumable dependency-aware plan." },
    { src: "assets/site/gpo-studio.webp", title: "Group Policy Studio", alt: "Group Policy Studio", caption: "Search real ADMX and ADML catalogs and edit values through schema-derived controls." },
    { src: "assets/site/history-time-machine.webp", title: "History Time Machine", alt: "History Time Machine", caption: "Trace, compare, selectively undo, and restore without deleting the original decision trail." },
    { src: "assets/site/safety-guardrails.webp", title: "Safety guardrails", alt: "Safety guardrails", caption: "Protect source media, verify hashes, checkpoint operations, and publish outputs atomically." },
    { src: "assets/site/automation-cli.webp", title: "Automation CLI", alt: "Automation CLI", caption: "Drive the same contracts with deterministic JSON, stable exit codes, and explicit confirmations." },
    { src: "assets/site/workflow-overview.webp", title: "Complete workflow", alt: "WimForge workflow overview", caption: "Move from source to recipe, review, run, and disposable virtual-machine validation." }
  ];

  const lightbox = document.querySelector("[data-lightbox]");
  const lightboxImage = lightbox?.querySelector("[data-lightbox-image]");
  const lightboxTitle = lightbox?.querySelector("[data-lightbox-title]");
  const lightboxCaption = lightbox?.querySelector("[data-lightbox-caption]");
  const lightboxCount = lightbox?.querySelector("[data-lightbox-count]");
  let galleryIndex = 0;
  let galleryTrigger = null;

  const showGalleryItem = (index) => {
    galleryIndex = (index + gallery.length) % gallery.length;
    const item = gallery[galleryIndex];
    if (!lightboxImage || !lightboxTitle || !lightboxCaption || !lightboxCount) return;
    lightboxImage.closest(".media-shell")?.classList.remove("is-image-ready", "is-image-missing");
    lightboxImage.src = item.src;
    lightboxImage.alt = item.alt;
    lightboxTitle.textContent = item.title;
    lightboxCaption.textContent = item.caption;
    lightboxCount.textContent = `${galleryIndex + 1} / ${gallery.length}`;
    if (lightboxImage.complete) settleImage(lightboxImage);
  };

  const openLightbox = (index, trigger) => {
    if (!lightbox) return;
    galleryTrigger = trigger;
    showGalleryItem(index);
    lightbox.showModal();
    document.body.style.overflow = "hidden";
  };

  const closeLightbox = () => {
    if (!lightbox?.open) return;
    lightbox.close();
    document.body.style.overflow = "";
    galleryTrigger?.focus();
  };

  document.querySelectorAll("[data-lightbox-index]").forEach((button) => {
    button.addEventListener("click", () => openLightbox(Number(button.dataset.lightboxIndex), button));
  });
  lightbox?.querySelector("[data-lightbox-close]")?.addEventListener("click", closeLightbox);
  lightbox?.querySelector("[data-lightbox-previous]")?.addEventListener("click", () => showGalleryItem(galleryIndex - 1));
  lightbox?.querySelector("[data-lightbox-next]")?.addEventListener("click", () => showGalleryItem(galleryIndex + 1));
  lightbox?.addEventListener("click", (event) => { if (event.target === lightbox) closeLightbox(); });
  lightbox?.addEventListener("cancel", (event) => { event.preventDefault(); closeLightbox(); });
  document.addEventListener("keydown", (event) => {
    if (!lightbox?.open) return;
    if (event.key === "ArrowLeft") showGalleryItem(galleryIndex - 1);
    if (event.key === "ArrowRight") showGalleryItem(galleryIndex + 1);
  });

  document.querySelectorAll("[data-year]").forEach((item) => { item.textContent = String(new Date().getFullYear()); });
})();
