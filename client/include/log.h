#ifndef LOG_H
#define LOG_H

/**
 * @file log.h
 * @ingroup pc_client
 * @brief Zero-dependency logging front end for the Linux PC-side tools.
 *
 * Mirrors the firmware's @c PRINT_MSG(level, fmt, ...) call style so both sides of the
 * project read the same way. Writes plain text to the standard streams and lets the init
 * system decide where it goes -- no journal/syslog dependency, so the same binary works
 * under systemd, OpenRC, runit, s6, or a bare shell.
 *
 * ### Stream split (Unix convention)
 * - @c LOG_LVL_INFO and @c LOG_LVL_DEBUG go to @c stdout (normal operation),
 * - @c LOG_LVL_WARN and @c LOG_LVL_ERROR go to @c stderr (problems).
 * This lets an operator separate faults for free, e.g. @c "daemon 2>errors.log".
 *
 * ### Init-system routing (the daemon does NOT choose where logs land)
 * The program only writes to fd 1 / fd 2. The service definition redirects them:
 * - systemd: captured by journald automatically (@c StandardOutput=journal);
 * - OpenRC:  @c supervise-daemon redirects to a logfile;
 * - runit/s6: the @c run script pipes stdout into @c svlogd / @c s6-log;
 * - SysV:    shell redirection @c ">>/var/log/app.log 2>&1".
 *
 * ### Color (interactive utility vs daemon)
 * Color is emitted ONLY when the target stream is a real terminal (@c isatty). Under an
 * init system the streams are pipes/files, so output is automatically plain -- no escape
 * code garbage in journald or logfiles. The interactive provisioning utility, run from a
 * shell, gets colored output. Same code, decided at runtime. Honors @c NO_COLOR
 * (https://no-color.org) too.
 *
 * ### Two gates (same model as the firmware)
 * - Runtime level: ::log_set_level (default @c LOG_LVL_INFO) hides quieter lines.
 * - Compile ceiling: define @c LOG_COMPILE_LEVEL before including to strip quieter lines
 *   at build time (e.g. @c -DLOG_COMPILE_LEVEL=LOG_LVL_INFO drops all DEBUG calls from
 *   the binary).
 *
 * ### Usage
 * @code
 * #define LOG_TAG "wifi"
 * #include "log.h"            // in every .c file that logs
 *
 * #define LOG_IMPLEMENTATION  // in exactly ONE .c file, before the include
 * #include "log.h"
 * @endcode
 *
 * @note Not a library -- a single self-contained header. Include it and go.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @name Severity levels
 * @brief Mirror the firmware's five levels so both sides match.
 *
 * Ordered so that a numerically higher value is quieter-by-default: a line is emitted
 * only when its level is @c <= the active ceiling, so raising the ceiling reveals more.
 * @{
 */
#define		LOG_LVL_ERROR		0	/**< Error: an operation failed. */
#define		LOG_LVL_WARN		1	/**< Warning: unexpected but recoverable. */
#define		LOG_LVL_INFO		2	/**< Info: normal operational milestone. */
#define		LOG_LVL_DEBUG		3	/**< Debug: developer-level detail. */
#define		LOG_LVL_VERBOSE		4	/**< Verbose: ultra-fine tracing. */
/** @} */

/**
 * @brief Compile-time ceiling. Lines above this are removed by the preprocessor.
 *
 * This is a build-time gate, not a runtime one: calls above the ceiling are compiled out
 * entirely, so they cost nothing (not even a branch) in the shipped binary. Override with
 * @c -DLOG_COMPILE_LEVEL=LOG_LVL_INFO for release builds to strip every DEBUG and VERBOSE
 * call.
 */
#ifndef     LOG_COMPILE_LEVEL
#define		LOG_COMPILE_LEVEL	LOG_LVL_VERBOSE
#endif

/**
 * @name Context defaults
 * @brief Level used when neither a CLI flag nor @c LOG_LEVEL is given.
 *
 * Selected automatically by ::log_is_interactive. Both default to @c LOG_LVL_INFO;
 * override at build time if different behaviour is wanted, e.g.
 * @c -DLOG_DEFAULT_DAEMON=LOG_LVL_DEBUG.
 *
 * @c LOG_LVL_INFO is the conventional choice for both: verbosity should be opt-in, since
 * a daemon runs continuously and DEBUG-by-default would flood the journal.
 * @{
 */
#ifndef     LOG_DEFAULT_DAEMON
#define		LOG_DEFAULT_DAEMON	LOG_LVL_INFO	/**< No TTY: running under an init system. */
#endif
#ifndef     LOG_DEFAULT_SHELL
#define		LOG_DEFAULT_SHELL	LOG_LVL_INFO	/**< TTY present: launched by a human. */
#endif
/** @} */

/**
 * @brief Process-wide runtime level, shared by every translation unit.
 *
 * Declared @c extern so all .c files see ONE level. Exactly one file must define the
 * storage by doing:
 * @code
 * #define LOG_IMPLEMENTATION
 * #include "log.h"          // in main.c, once
 * @endcode
 * Every other file just includes the header normally.
 *
 * @warning If this were @c static, each .c file would get its own private copy:
 *          ::log_init in main.c would set only main's copy, and other files would keep
 *          logging at the default level. The @c extern is what makes @c -v affect the
 *          whole program.
 */
extern int _log_runtime_level;

#ifdef      LOG_IMPLEMENTATION
int _log_runtime_level = LOG_LVL_INFO;
#endif

/**
 * @brief Set the runtime verbosity ceiling (default @c LOG_LVL_INFO).
 * @param level One of the @c LOG_LVL_* values; lines above it are suppressed.
 */
static inline void log_set_level(int level){
	_log_runtime_level = level;
}

/**
 * @brief Read the @c LOG_LEVEL environment variable and apply it, if present.
 *
 * Accepts @c error / @c warn / @c info / @c debug / @c verbose (and @c trace as a synonym
 * for @c verbose), case-insensitively. Lets an operator raise verbosity without
 * recompiling, e.g. @c "LOG_LEVEL=debug daemon", and is how a service unit sets the level
 * for a daemon that has no CLI to speak of.
 *
 * An absent or unrecognised value leaves the current level untouched, so a typo degrades
 * to the default rather than to silence.
 *
 * @note Useful on its own for a program whose CLI cannot spare an argument for log flags:
 *       call this instead of ::log_init and the level is still operator-controllable.
 */
static inline void log_init_from_env(void){
	const char *e = getenv("LOG_LEVEL");
	if(!e){
		return;
	}

	if(!strcasecmp(e, "error")){
		_log_runtime_level = LOG_LVL_ERROR;
	}else 
    if(!strcasecmp(e, "warn")){
		_log_runtime_level = LOG_LVL_WARN;
	}else 
    if(!strcasecmp(e, "info")){
		_log_runtime_level = LOG_LVL_INFO;
	}else 
    if(!strcasecmp(e, "debug")){
		_log_runtime_level = LOG_LVL_DEBUG;
	}else 
    if(!strcasecmp(e, "verbose") || !strcasecmp(e, "trace")){
		_log_runtime_level = LOG_LVL_VERBOSE;
	}
}

/**
 * @brief Report whether the process is attached to a terminal (interactive).
 *
 * Init systems (systemd, OpenRC, runit, s6, SysV) never give a service a controlling
 * terminal -- they hand it pipes or files. A human running the program from a shell
 * always has one. So this single check distinguishes "running as a daemon" from "run by a
 * human", with no init-system-specific code and no cooperation needed from the service
 * file.
 *
 * @return Non-zero if a terminal is attached (launched from a shell); zero if not
 *         (running under an init system as a daemon).
 */
static inline int log_is_interactive(void){
	return isatty(STDOUT_FILENO);
}

/**
 * @brief Report whether @p arg is a logging flag that ::log_init consumes.
 *
 * Exists so a program can walk its own @c argv and skip the arguments that belong to the
 * logging layer, without hardcoding (and thus duplicating) the list of accepted forms.
 *
 * This matters for a CLI with a strict "exactly one option" rule: such a program must be
 * able to tell a log flag apart from its real option verb, or @c "prog --pc=123456 -v"
 * would be rejected as two arguments. It scans, skips whatever this returns true for, and
 * requires exactly one thing left over.
 *
 * Recognised forms, matching ::log_init exactly:
 * - @c -v            (debug)
 * - @c -vv           (verbose)
 * - @c -q            (errors only)
 * - @c --log-level=X (explicit; X per ::log_init)
 *
 * @note The short flags are matched exactly, not by prefix, so @c -vvv is NOT a log flag
 *       and would be treated by the caller as a normal argument.
 *
 * @warning ::log_init and this function must agree. ::log_init gates on this function, so
 *          adding a flag here is half the job -- add the matching level assignment there
 *          too, and do not reintroduce a separate copy of this list in a caller.
 *
 * @param arg A single argument string from @c argv.
 * @return Non-zero if @p arg is a logging flag; zero if it belongs to the caller.
 */
static inline int log_is_log_arg(const char *arg){
	if(!arg){
		return 0;
	}

	if(!strcmp(arg, "-v")){
		return 1;
	}

	if(!strcmp(arg, "-vv")){
		return 1;
	}

	if(!strcmp(arg, "-q")){
		return 1;
	}

	if(!strncmp(arg, "--log-level=", 12)){
		return 1;
	}

	return 0;
}

/**
 * @brief One-call setup: pick the runtime level, honouring every override.
 *
 * Precedence, highest first:
 *   1. CLI flag  (@c -v, @c -vv, @c -q, @c --log-level=X)
 *   2. @c LOG_LEVEL environment variable (this is how a service unit sets it)
 *   3. Context default, chosen by ::log_is_interactive:
 *        - daemon (no TTY) -> @c LOG_DEFAULT_DAEMON
 *        - shell  (TTY)    -> @c LOG_DEFAULT_SHELL
 *
 * The ordering is correct by construction: the three sources are applied lowest-first, so
 * each simply overwrites the one before it. This is the single entry point, so call it
 * once at the top of @c main() and nothing else needs to reason about precedence.
 *
 * @c --log-level=X accepts @c error / @c warn / @c info / @c debug / @c verbose (and
 * @c trace as a synonym for @c verbose), case-insensitively, matching
 * ::log_init_from_env. An unrecognised value is ignored and the level is left as-is.
 *
 * Arguments this function does not recognise are silently skipped -- parse them with the
 * program's own option handling as usual. A program that must distinguish its own options
 * from these can use ::log_is_log_arg.
 *
 * @note There is no precedence among the CLI flags themselves; the loop runs to the end
 *       of @c argv and the last recognised flag wins. So @c "prog -q -v" ends at DEBUG,
 *       not ERROR.
 *
 * @param argc Argument count from @c main().
 * @param argv Argument vector from @c main().
 */
static inline void log_init(int argc, char **argv){
	/* 3. context default (lowest priority) */
	_log_runtime_level = log_is_interactive() ? LOG_DEFAULT_SHELL : LOG_DEFAULT_DAEMON;

	/* 2. env overrides the context default */
	log_init_from_env();

	/* 1. CLI overrides both */
	for(int i = 1; i < argc; i++){
		const char *a = argv[i];

		if(!log_is_log_arg(a)){
			continue;
		}

		if(!strcmp(a, "-v")){
			_log_runtime_level = LOG_LVL_DEBUG;
		}else 
        if(!strcmp(a, "-vv")){
			_log_runtime_level = LOG_LVL_VERBOSE;
		}else 
        if(!strcmp(a, "-q")){
			_log_runtime_level = LOG_LVL_ERROR;
		}else{
			const char *v = a + 12;		/* past "--log-level=" */

			if(!strcasecmp(v, "error")){
				_log_runtime_level = LOG_LVL_ERROR;
			}else 
            if(!strcasecmp(v, "warn")){
				_log_runtime_level = LOG_LVL_WARN;
			}else 
            if(!strcasecmp(v, "info")){
				_log_runtime_level = LOG_LVL_INFO;
			}else 
            if(!strcasecmp(v, "debug")){
				_log_runtime_level = LOG_LVL_DEBUG;
			}else 
            if(!strcasecmp(v, "verbose") || !strcasecmp(v, "trace")){
				_log_runtime_level = LOG_LVL_VERBOSE;
			}
		}
	}
}

/** @brief Per-level display metadata: label, ANSI color, and target stream. */
struct _log_meta {
	const char	*label;
	const char	*color;
	FILE		*stream;
};

/**
 * @brief Look up the label, color and stream for a level.
 *
 * INFO/DEBUG go to stdout; WARN/ERROR go to stderr. Colors: error red, warn yellow, info
 * green, debug dim, trace dim magenta. Labels are padded to five characters so the
 * message column stays aligned across levels.
 *
 * @param level One of the @c LOG_LVL_* values; anything else falls through to TRACE.
 * @return The metadata for @p level.
 */
static inline struct _log_meta _log_meta_for(int level){
	switch(level){
		case LOG_LVL_ERROR:
			return (struct _log_meta){ "ERROR", "\033[31m", stderr };

		case LOG_LVL_WARN:
			return (struct _log_meta){ "WARN ", "\033[33m", stderr };

		case LOG_LVL_INFO:
			return (struct _log_meta){ "INFO ", "\033[32m", stdout };

		case LOG_LVL_DEBUG:
			return (struct _log_meta){ "DEBUG", "\033[2m", stdout };

		default:
			return (struct _log_meta){ "TRACE", "\033[2;35m", stdout };
	}
}

/**
 * @brief Decide whether to colorize @p stream: only if it is a TTY and @c NO_COLOR is unset.
 *
 * Checked per stream rather than per process, since stdout and stderr can be redirected
 * independently (@c "daemon 2>errors.log" leaves stdout on the terminal).
 *
 * @param stream Destination (@c stdout or @c stderr).
 * @return Non-zero if color escapes should be emitted.
 */
static inline int _log_use_color(FILE *stream){
	if(getenv("NO_COLOR")){
		return 0;
	}

	return isatty(fileno(stream));
}

/**
 * @brief Core emit: timestamp + level + tag + message, to the level's stream.
 *
 * Format: @c "2026-07-10T14:22:01Z LEVEL tag: message". The timestamp is UTC ISO 8601 --
 * systemd/journald add their own, but this keeps standalone and file output
 * self-describing. Color wraps only the level label, and only on a TTY.
 *
 * The stream is flushed on every line. That costs throughput, but it means the last line
 * before a crash is actually on disk rather than lost in a stdio buffer -- which is the
 * line that usually matters.
 *
 * @note Level filtering happens here at runtime AND in ::PRINT_MSG at compile time. This
 *       one guards against a level raised at runtime; that one removes the call entirely.
 *
 * @param level One of the @c LOG_LVL_* values.
 * @param tag   Short component tag (e.g. "wifi", "prov"); may be NULL.
 * @param fmt   printf-style format (no trailing newline; this adds one).
 * @param ...   Arguments for @p fmt.
 */
static inline void _log_emit(int level, const char *tag, const char *fmt, ...){
	if(level > _log_runtime_level){
		return;
	}

	struct _log_meta m = _log_meta_for(level);

	char ts[32];
	time_t now = time(NULL);
	struct tm tm_utc;

	gmtime_r(&now, &tm_utc);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

	int color = _log_use_color(m.stream);

	if(color){
		fprintf(m.stream, "%s %s%s\033[0m ", ts, m.color, m.label);
	}else{
		fprintf(m.stream, "%s %s ", ts, m.label);
	}

	if(tag){
		fprintf(m.stream, "%s: ", tag);
	}

	va_list ap;
	va_start(ap, fmt);
	vfprintf(m.stream, fmt, ap);
	va_end(ap);

	fputc('\n', m.stream);
	fflush(m.stream);
}

/**
 * @brief Emit a log line at @p lvl. Mirrors the firmware's @c PRINT_MSG.
 *
 * The @c if is evaluated at build time against @c LOG_COMPILE_LEVEL, so lines above the
 * ceiling are removed by the compiler and cost nothing in the binary -- not even the
 * runtime level check inside ::_log_emit.
 *
 * @note Wrapped in @c do/while(0) so it behaves as a single statement: an unbraced
 *       @c "if(x) PRINT_MSG(...); else ..." would otherwise fail to compile.
 *
 * @param lvl One of the @c LOG_LVL_* values.
 * @param fmt printf-style format string.
 * @param ... Arguments for @p fmt.
 */
#define     PRINT_MSG(lvl, fmt, ...) \
	        do { if((lvl) <= LOG_COMPILE_LEVEL){ _log_emit((lvl), LOG_TAG, fmt, ##__VA_ARGS__); } } while(0)

/**
 * @brief Per-file component tag. Define before including to name the component:
 * @code
 * #define LOG_TAG "wifi"
 * #include "log.h"
 * @endcode
 * Falls back to NULL (no tag prefix) if undefined.
 */
#ifndef     LOG_TAG
#define		LOG_TAG		NULL
#endif

#endif	/* LOG_H */
