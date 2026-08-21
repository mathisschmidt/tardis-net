/**
 * Front-end glue.
 *
 * htmx owns every network call; Alpine owns rendering. Server state arrives as
 * an `HX-Trigger: tardis:state` event with an empty response body, so the
 * console is updated in place and its CSS animations never restart.
 */
(function () {
  "use strict";

  const EMPTY_MACHINE = {
    name: "tardis",
    status: "offline",
    label: "Offline",
    description: "",
    is_on: false,
    linked: false,
    transitioning: false,
    progress: 0,
    eta_seconds: 0,
    changed_at: null,
    since_seconds: 0,
    boot_count: 0,
  };

  document.addEventListener("alpine:init", () => {
    Alpine.store("tardis", {
      machine: { ...EMPTY_MACHINE },
      prefs: { confirm_before_power_off: true, animations: true, poll_interval: 3 },
      toast: "",
      toastTone: "info",
      pendingConfirm: null,
      _toastTimer: null,
      _pollTimer: null,
      _tick: null,

      /** Seed from the server-rendered page, then start polling. */
      hydrate(payload) {
        this.apply(payload);
        this.startPolling();
        // Keeps "3 minutes ago" and the transition countdown honest between polls.
        this._tick = setInterval(() => {
          if (this.machine.changed_at) {
            this.machine.since_seconds = Math.max(
              0,
              Math.floor(Date.now() / 1000 - this.machine.changed_at)
            );
          }
          if (this.machine.transitioning && this.machine.eta_seconds > 0) {
            this.machine.eta_seconds -= 1;
          }
        }, 1000);
      },

      apply(payload) {
        if (!payload) return;
        if (payload.machine) this.machine = payload.machine;
        if (payload.preferences) {
          const next = payload.preferences;
          const intervalChanged = next.poll_interval !== this.prefs.poll_interval;
          this.prefs = next;
          if (intervalChanged) this.startPolling();
        }
        if (payload.error) this.notify(payload.error, "error");
        else if (payload.toast) this.notify(payload.toast, "info");
      },

      notify(message, tone) {
        this.toast = message;
        this.toastTone = tone || "info";
        clearTimeout(this._toastTimer);
        this._toastTimer = setTimeout(() => (this.toast = ""), 4000);
      },

      /** Poll through htmx so responses flow through the same event path. */
      startPolling() {
        clearInterval(this._pollTimer);
        const seconds = Math.min(60, Math.max(1, Number(this.prefs.poll_interval) || 3));
        this._pollTimer = setInterval(() => {
          if (document.hidden) return;
          htmx.ajax("GET", "/partials/state", { swap: "none" });
        }, seconds * 1000);
      },

      nextAction() {
        return this.machine.is_on ? "off" : "on";
      },

      actionLabel() {
        if (this.machine.transitioning) return this.machine.label;
        return this.machine.is_on ? `Power off ${this.machine.name}` : `Power on ${this.machine.name}`;
      },

      confirmPower() {
        const pending = this.pendingConfirm;
        this.pendingConfirm = null;
        if (pending) pending.issueRequest(true);
      },

      cancelPower() {
        this.pendingConfirm = null;
      },

      // Without a recent hardware poll, the cached power state is unverified —
      // the pill says so instead of asserting online/offline.
      pillLabel() {
        return this.machine.linked ? this.machine.label : "Not linked";
      },

      get pillClass() {
        if (!this.machine.linked) return "status-pill-offline";
        return {
          online: "status-pill-online",
          offline: "status-pill-offline",
          booting: "status-pill-busy",
          shutting_down: "status-pill-busy",
        }[this.machine.status];
      },

      get dotClass() {
        if (!this.machine.linked) return "bg-slate-500";
        return {
          online: "bg-emerald-400",
          offline: "bg-slate-500",
          booting: "bg-amber-300",
          shutting_down: "bg-amber-300",
        }[this.machine.status];
      },

      sinceLabel() {
        return this.machine.is_on ? `Online for ${formatDuration(this.machine.since_seconds)}` :
          `Idle for ${formatDuration(this.machine.since_seconds)}`;
      },
    });

    /** Six single-character boxes backed by one hidden `code` field. */
    Alpine.data("otpForm", () => ({
      digits: ["", "", "", "", "", ""],
      submitting: false,
      copied: false,

      get code() {
        return this.digits.join("");
      },

      boxes() {
        // `$root` — not `$el`, which is whichever input triggered the handler.
        return Array.from(this.$root.querySelectorAll(".otp-box"));
      },

      focusBox(index) {
        const box = this.boxes()[index];
        if (box) {
          box.focus();
          box.select();
        }
      },

      /** The inputs hold their own text; `digits` mirrors them for the hidden field. */
      setDigit(index, char) {
        if (index < 0 || index > 5) return;
        this.digits[index] = char;
        const box = this.boxes()[index];
        if (box) box.value = char;
      },

      onInput(index, event) {
        const typed = event.target.value.replace(/\D/g, "");
        if (!typed) {
          this.setDigit(index, "");
          return;
        }
        // A burst of digits (autofill, fast typing) spills into the next boxes.
        typed.split("").forEach((char, offset) => this.setDigit(index + offset, char));
        this.focusBox(Math.min(5, index + typed.length));
        this.maybeSubmit();
      },

      onKeydown(index, event) {
        if (event.key === "Backspace") {
          event.preventDefault();
          if (this.digits[index]) {
            this.setDigit(index, "");
          } else if (index > 0) {
            this.setDigit(index - 1, "");
            this.focusBox(index - 1);
          }
        } else if (event.key === "ArrowLeft" && index > 0) {
          event.preventDefault();
          this.focusBox(index - 1);
        } else if (event.key === "ArrowRight" && index < 5) {
          event.preventDefault();
          this.focusBox(index + 1);
        }
      },

      onPaste(event) {
        const text = (event.clipboardData || window.clipboardData).getData("text") || "";
        const pasted = text.replace(/\D/g, "").slice(0, 6).split("");
        for (let i = 0; i < 6; i += 1) this.setDigit(i, pasted[i] || "");
        this.focusBox(Math.min(5, pasted.length));
        this.maybeSubmit();
      },

      maybeSubmit() {
        if (this.code.length !== 6 || this.submitting) return;
        // Wait for the hidden `code` field to catch up before htmx serialises it.
        this.$nextTick(() => {
          const form = this.$refs.form;
          if (!form) return;
          if (typeof form.requestSubmit === "function") form.requestSubmit();
          else htmx.trigger(form, "submit");
        });
      },

      async copySecret(secret) {
        try {
          await navigator.clipboard.writeText(secret);
          this.copied = true;
          setTimeout(() => (this.copied = false), 2000);
        } catch (err) {
          // Clipboard is blocked outside secure contexts — the key is on screen anyway.
        }
      },
    }));
  });

  function formatDuration(totalSeconds) {
    const seconds = Math.max(0, Math.floor(totalSeconds || 0));
    if (seconds < 60) return `${seconds}s`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `${minutes}m ${seconds % 60}s`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return `${hours}h ${minutes % 60}m`;
    return `${Math.floor(hours / 24)}d ${hours % 24}h`;
  }

  // ---------------------------------------------------------------- htmx wiring

  document.addEventListener("DOMContentLoaded", () => {
    // State pushed from the server on polls, power commands and pref saves.
    document.body.addEventListener("tardis:state", (event) => {
      const detail = event.detail || {};
      const payload = detail.machine ? detail : detail.value;
      if (window.Alpine) Alpine.store("tardis").apply(payload);
    });

    // Route the power button through the styled dialog when asked to confirm.
    document.body.addEventListener("htmx:confirm", (event) => {
      const element = event.detail.elt;
      if (!element || !element.hasAttribute("data-confirm-power")) return;
      const store = window.Alpine && Alpine.store("tardis");
      if (!store || !store.prefs.confirm_before_power_off || !store.machine.is_on) return;
      event.preventDefault();
      store.pendingConfirm = event.detail;
    });

    document.body.addEventListener("htmx:responseError", (event) => {
      if (!window.Alpine) return;
      const status = event.detail.xhr && event.detail.xhr.status;
      if (status === 401) return; // The server redirects us to /login.
      Alpine.store("tardis").notify("The server rejected that request.", "error");
    });

    document.body.addEventListener("htmx:sendError", () => {
      if (window.Alpine) Alpine.store("tardis").notify("Server unreachable.", "error");
    });
  });
})();
