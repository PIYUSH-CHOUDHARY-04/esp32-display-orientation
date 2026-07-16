# `service/` — starting the daemon at login

One route: **XDG autostart**. `./install.sh install` sets it up. You shouldn't need to touch
anything here, but this explains what it did and why.

```
service/
└── autostart/
    ├── esp_daemon.desktop      → ~/.config/autostart/esp_daemon.desktop
    └── esp_daemon-session      → ~/.local/bin/esp_daemon-session
```

| PID 1 is...   | Route used    | Root? |
| ------------- | ------------- | ----- |
| anything      | XDG autostart | no    |

**The route needs no `sudo`.** Everything lands under `$HOME`.

---

## The problem it's solving

The daemon rotates your screen by running `xrandr`. `xrandr` talks to the **X server**, and X does
not talk to just anybody. It wants two things:

| Variable     | What it answers                                                      |
| ------------ | -------------------------------------------------------------------- |
| `DISPLAY`    | *Which* X server? (usually `:0`)                                     |
| `XAUTHORITY` | *Prove you're allowed to.* Path to your `~/.Xauthority` cookie file. |

When you run the daemon from your own terminal it already has both — your shell inherited them at
login. That's why it works when you test it by hand.

**A service started at boot has neither.** It isn't part of anyone's session. No `DISPLAY`, no
cookie, and usually no X server at all, because nobody has logged in yet. So `xrandr` runs, X
refuses the connection, and the rotation fails.

The failure is nasty, because it's *silent*: the daemon finds the ESP, connects, receives every
command, acknowledges every command — and the screen never moves. Everything in the log looks
healthy.

**So the daemon must start inside the session, as you.** XDG autostart does exactly that.

---

## Why XDG autostart, and not anything else

### Not a system init script (OpenRC, runit, s6, SysV)

Because **they can't do this.** They're *system* supervisors: they start things at **boot**, as
**root**, when nobody is logged in and no X server exists. A service they start has no cookie and
can't talk to X.

And they offer no login hook to use instead — they have no concept of a login at all. The display
manager owns that, long after init has finished.

You *can* work around it: hardcode a username and a cookie path into `/etc/init.d/esp_daemon`. But
that costs root, bakes an X cookie path into a machine-wide file, and allows exactly **one user per
machine** — there's only one `/etc/init.d/esp_daemon`, so the second person to install overwrites
the first.

### Not a systemd user unit either

This one is more subtle, because a systemd *user* unit genuinely *can* express "start inside the
session" — that's what `graphical-session.target` is for, and on GNOME and KDE it works well, with
real supervision (`Restart=on-failure` with backoff, a crash-loop limit, journald) as a bonus.

The catch is portability. `graphical-session.target` is a **convention, not a guarantee**: the
desktop is *supposed* to activate it so user services can bind to the session's lifetime. GNOME and
KDE do. **Cinnamon, XFCE, i3, sway and others frequently do not** — they start a perfectly good X
session and never tell systemd about it. On those systems the unit sits inert while
`systemctl --user enable` reports success and plants a symlink that's never read. The daemon appears
enabled and never starts — at install, at login, ever — and every command you'd think to run says
the right thing. A silent failure that looks like success is the worst kind.

Binding to `default.target` instead only trades one silent failure for another: that target is
reached for an SSH login too, with no `DISPLAY`, so the unit fires with nothing to rotate.

So a systemd route would behave differently on different desktops. Rather than ship that
inconsistency, the installer ships the one mechanism that behaves the same everywhere.

### What autostart does instead

`~/.config/autostart/` is read by the **desktop session**, after the display manager has
authenticated you, started X, and built your environment. The daemon starts **as you**, **in your
session**, with `DISPLAY` and `XAUTHORITY` already present.

- **No root.** Nothing in `/etc`.
- **Nothing substituted.** It's already running as the right user.
- **Per-user.** The file is in *your* home. Two people on one machine each get their own; neither
  can overwrite the other.
- **In step with the session.** The session reads the directory itself, so the daemon's lifetime
  matches the session's on every desktop — no target that might or might not be activated.

> **This isn't desktop-specific.** The name misleads. XDG autostart is a freedesktop.org standard —
> GNOME, KDE, XFCE, LXDE, MATE, Cinnamon, i3, sway all implement it. One file, every desktop. The
> dependency is on *having* a graphical session, which you need anyway, since `xrandr` is what does
> the rotating.

### The wrapper, and why it exists

`Exec=` in a `.desktop` file is parsed by the **desktop**, not a shell. Loops, pipes and redirection
either don't work or get mangled differently by different implementations. So `Exec=` points at a
real shell script, and that's where the restart loop lives:

```sh
Exec=/home/you/.local/bin/esp_daemon-session
```

The wrapper runs the daemon and restarts it if it dies unexpectedly. Two details in it are
load-bearing:

**It traps SIGTERM.** At logout the session signals everything it started. Without the trap, a naive
`while :; do daemon; done` would see the daemon die from that signal, call it a crash, and spawn a
*fresh* daemon into a session being torn down. The trap kills the child and **breaks** the loop
instead.

**It backgrounds the daemon and `wait`s.** A foreground child makes the shell deaf to signals until
that child exits — so the trap would never fire.

```sh
pkill -f esp_daemon-session                 # stop it now
rm ~/.config/autostart/esp_daemon.desktop   # disable it permanently
```

### What you give up, and why it doesn't matter

Real supervision. A systemd unit would give you `Restart=on-failure` with backoff, a crash-loop
limit that gives up after 5 failures in 60s, and journald. The wrapper is a `while` loop — if the
daemon failed *permanently* (missing `xrandr`, say), systemd would stop trying; the wrapper restarts
it every 2 seconds until you log out.

In practice this rarely matters. The daemon's own main loop already recovers from every network
failure internally — a vanished ESP, a changed IP, a dropped link are all handled without exiting.
The wrapper is a backstop against a crash that shouldn't happen, not a crutch. The one hard-failure
case (`xrandr` genuinely missing) is loud in the log rather than silent, and is a
fix-it-once problem, not a supervision problem.

---

## Where your logs go

The daemon writes plain text to **stdout** and **stderr** and lets whatever started it decide where
that lands. That's deliberate (see `client/include/log.h`): no journald dependency, no syslog
dependency, one binary that works everywhere.

It splits the streams the usual Unix way:

- `INFO` and `DEBUG` → **stdout**
- `WARN` and `ERROR` → **stderr**

Because the **desktop session** is what launches the wrapper, the daemon's output goes wherever your
session sends the output of the things it starts. On the large majority of desktops that is:

```
~/.xsession-errors
```

```sh
tail -f ~/.xsession-errors            # follow the daemon live
grep esp_daemon ~/.xsession-errors    # just its lines
```

That file isn't guaranteed by a standard, so a few setups differ:

| Your setup                                   | Where session output lands                               |
| -------------------------------------------- | ---------------------------------------------------------|
| Most desktops (GNOME/KDE/XFCE/… on X)        | `~/.xsession-errors`                                     |
| systemd-logind desktops that capture it      | the journal: `journalctl --user` (grep for `esp_daemon`) |
| A bare `.xinitrc` / `startx`                  | the TTY you launched X from                             |
| A session that discards child output         | nowhere — you see nothing                                |

If you land in the last row and want a guaranteed location, redirect the daemon yourself in the
wrapper (`~/.local/bin/esp_daemon-session`), just above the `while` loop — for example:

```sh
exec >>"$HOME/.local/state/esp_daemon.log" 2>&1
```

That folds both streams into one file you control, on every desktop. It's not the default because
the default deliberately imposes no log policy — it lets your session decide, the same way every
other autostarted program does.

### Turning up the verbosity

No recompiling. `log.h` reads `LOG_LEVEL`:

```
error | warn | info | debug | verbose
```

Add it to the wrapper, above the `while` loop:

```sh
export LOG_LEVEL=debug
```

Then log out and back in (or `pkill -f esp_daemon-session` and re-run the wrapper).

---

## Checking it worked

**A healthy log looks like this:**

```
INFO  main: starting orientation daemon
INFO  main: external display output: HDMI1
INFO  discover_esp: ESP found on wlo1 at 192.168.1.42:6000
INFO  start_tcp_client: connected to ESP, waiting for orientation commands
INFO  __rotate_display__: display rotated to normal
```

**And here's the failure you're most likely to hit:**

```
ERROR __rotate_display__: xrandr exited 1, rotation not applied
```

Everything else in the log looks fine — the ESP is found, the connection is up, commands arrive —
and the screen simply doesn't turn. Two causes, in order of likelihood:

**1. You're on Wayland.** `xrandr` can't rotate a Wayland output; the compositor owns it, not X.

```sh
echo $XDG_SESSION_TYPE      # "wayland" → this is your problem
```

Log in to an X11/Xorg session instead. Most display managers offer this from a gear icon on the
login screen. (The `.desktop` entry sets `OnlyShowIn=` to X11 desktops, so on a pure Wayland
session it won't even start — but XWayland setups can slip through.)

**2. X is refusing the connection.** Shouldn't happen on the autostart route, since it runs inside
your session — but if you're running the daemon by hand from `cron`, `ssh`, or a system service you
wrote yourself, check that it actually has a cookie:

```sh
echo $DISPLAY $XAUTHORITY
ls -l ~/.Xauthority
```

---

## Rolling your own

If you want to start the daemon some other way — a systemd user unit on a desktop where you *know*
`graphical-session.target` is activated, a `.xinitrc` line, a supervisor of your own — the contract
is four points:

1. **Run it as the desktop user**, not as root. `~/.local/bin/esp_daemon`
2. **Set the display** — `DISPLAY` is usually `:0`.
3. **Set the auth cookie** — `XAUTHORITY` is `/home/<user>/.Xauthority`. This is the step people forget, and the
   failure is silent.
4. **Capture stdout *and* stderr.** They're separate streams and both matter — errors go to stderr.
   If your supervisor only captures one, fold them: `exec 2>&1`.

Optionally: restart on failure, and set `LOG_LEVEL` for debug output.

`service/autostart/esp_daemon-session` is a working example of all four. Start from that.
