/*
 *  dload.c
 *
 *  Copyright (c) 2006-2020 Pacman Development Team <pacman-dev@archlinux.org>
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h> /* setsockopt, SO_KEEPALIVE */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h> /* IPPROTO_TCP */
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h> /* TCP_KEEPINTVL, TCP_KEEPIDLE */
#endif

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

/* libalpm */
#include "dload.h"
#include "alpm_list.h"
#include "alpm.h"
#include "log.h"
#include "util.h"
#include "handle.h"

#ifdef HAVE_LIBCURL
static const char *get_filename(const char *url)
{
	char *filename = strrchr(url, '/');
	if(filename != NULL) {
		return filename + 1;
	}

	/* no slash found, it's a filename */
	return url;
}

static char *get_fullpath(const char *path, const char *filename,
		const char *suffix)
{
	char *filepath;
	/* len = localpath len + filename len + suffix len + null */
	size_t len = strlen(path) + strlen(filename) + strlen(suffix) + 1;
	MALLOC(filepath, len, return NULL);
	snprintf(filepath, len, "%s%s%s", path, filename, suffix);

	return filepath;
}

enum {
	ABORT_SIGINT = 1,
	ABORT_OVER_MAXFILESIZE
};

static int dload_interrupted;
static void inthandler(int UNUSED signum)
{
	dload_interrupted = ABORT_SIGINT;
}

static int dload_progress_cb(void *file, curl_off_t dltotal, curl_off_t dlnow,
		curl_off_t UNUSED ultotal, curl_off_t UNUSED ulnow)
{
	struct dload_payload *payload = (struct dload_payload *)file;
	off_t current_size, total_size;
	alpm_download_event_progress_t cb_data = {0};

	/* do not print signature files progress bar */
	if(payload->signature) {
		return 0;
	}

	/* avoid displaying progress bar for redirects with a body */
	if(payload->respcode >= 300) {
		return 0;
	}

	/* SIGINT sent, abort by alerting curl */
	if(dload_interrupted) {
		return 1;
	}

	if(dlnow < 0 || dltotal <= 0 || dlnow > dltotal) {
		/* bogus values : stop here */
		return 0;
	}

	current_size = payload->initial_size + dlnow;

	/* is our filesize still under any set limit? */
	if(payload->max_size && current_size > payload->max_size) {
		dload_interrupted = ABORT_OVER_MAXFILESIZE;
		return 1;
	}

	/* none of what follows matters if the front end has no callback */
	if(payload->handle->dlcb == NULL) {
		return 0;
	}

	total_size = payload->initial_size + dltotal;

	if(payload->prevprogress == total_size) {
		return 0;
	}

	/* do NOT include initial_size since it wasn't part of the package's
	 * download_size (nor included in the total download size callback) */
	cb_data.total = dltotal;
	cb_data.downloaded = dlnow;
	payload->handle->dlcb(payload->remote_name, ALPM_DOWNLOAD_PROGRESS, &cb_data);
	payload->prevprogress = current_size;

	return 0;
}

static int curl_gethost(const char *url, char *buffer, size_t buf_len)
{
	size_t hostlen;
	char *p, *q;

	if(strncmp(url, "file://", 7) == 0) {
		p = _("disk");
		hostlen = strlen(p);
	} else {
		p = strstr(url, "//");
		if(!p) {
			return 1;
		}
		p += 2; /* jump over the found // */
		hostlen = strcspn(p, "/");

		/* there might be a user:pass@ on the URL. hide it. avoid using memrchr()
		 * for portability concerns. */
		q = p + hostlen;
		while(--q > p) {
			if(*q == '@') {
				break;
			}
		}
		if(*q == '@' && p != q) {
			hostlen -= q - p + 1;
			p = q + 1;
		}
	}

	if(hostlen > buf_len - 1) {
		/* buffer overflow imminent */
		return 1;
	}
	memcpy(buffer, p, hostlen);
	buffer[hostlen] = '\0';

	return 0;
}

static int utimes_long(const char *path, long seconds)
{
	if(seconds != -1) {
		struct timeval tv[2] = {
			{ .tv_sec = seconds, },
			{ .tv_sec = seconds, },
		};
		return utimes(path, tv);
	}
	return 0;
}

/* prefix to avoid possible future clash with getumask(3) */
static mode_t _getumask(void)
{
	mode_t mask = umask(0);
	umask(mask);
	return mask;
}

static size_t dload_parseheader_cb(void *ptr, size_t size, size_t nmemb, void *user)
{
	size_t realsize = size * nmemb;
	const char *fptr, *endptr = NULL;
	const char * const cd_header = "Content-Disposition:";
	const char * const fn_key = "filename=";
	struct dload_payload *payload = (struct dload_payload *)user;
	long respcode;

	if(_alpm_raw_ncmp(cd_header, ptr, strlen(cd_header)) == 0) {
		if((fptr = strstr(ptr, fn_key))) {
			fptr += strlen(fn_key);

			/* find the end of the field, which is either a semi-colon, or the end of
			 * the data. As per curl_easy_setopt(3), we cannot count on headers being
			 * null terminated, so we look for the closing \r\n */
			endptr = fptr + strcspn(fptr, ";\r\n") - 1;

			/* remove quotes */
			if(*fptr == '"' && *endptr == '"') {
				fptr++;
				endptr--;
			}

			STRNDUP(payload->content_disp_name, fptr, endptr - fptr + 1,
					RET_ERR(payload->handle, ALPM_ERR_MEMORY, realsize));
		}
	}

	curl_easy_getinfo(payload->curl, CURLINFO_RESPONSE_CODE, &respcode);
	if(payload->respcode != respcode) {
		payload->respcode = respcode;
	}

	return realsize;
}

static void curl_set_handle_opts(struct dload_payload *payload,
		CURL *curl, char *error_buffer)
{
	alpm_handle_t *handle = payload->handle;
	const char *useragent = getenv("HTTP_USER_AGENT");
	struct stat st;

	/* the curl_easy handle is initialized with the alpm handle, so we only need
	 * to reset the handle's parameters for each time it's used. */
	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, payload->fileurl);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dload_progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)payload);
	if(!handle->disable_dl_timeout) {
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
	}
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, dload_parseheader_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)payload);
	curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
	curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
	curl_easy_setopt(curl, CURLOPT_PRIVATE, (void *)payload);

	_alpm_log(handle, ALPM_LOG_DEBUG, "%s: url is %s\n",
		payload->remote_name, payload->fileurl);

	if(payload->max_size) {
		_alpm_log(handle, ALPM_LOG_DEBUG, "%s: maxsize %jd\n",
				payload->remote_name, (intmax_t)payload->max_size);
		curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
				(curl_off_t)payload->max_size);
	}

	if(useragent != NULL) {
		curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent);
	}

	if(!payload->allow_resume && !payload->force && payload->destfile_name &&
			stat(payload->destfile_name, &st) == 0) {
		/* start from scratch, but only download if our local is out of date. */
		curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
		curl_easy_setopt(curl, CURLOPT_TIMEVALUE, (long)st.st_mtime);
		_alpm_log(handle, ALPM_LOG_DEBUG,
				"%s: using time condition %ld\n",
				payload->remote_name, (long)st.st_mtime);
	} else if(stat(payload->tempfile_name, &st) == 0 && payload->allow_resume) {
		/* a previous partial download exists, resume from end of file. */
		payload->tempfile_openmode = "ab";
		curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)st.st_size);
		_alpm_log(handle, ALPM_LOG_DEBUG,
				"%s: tempfile found, attempting continuation from %jd bytes\n",
				payload->remote_name, (intmax_t)st.st_size);
		payload->initial_size = st.st_size;
	}
}

static void mask_signal(int signum, void (*handler)(int),
		struct sigaction *origaction)
{
	struct sigaction newaction;

	newaction.sa_handler = handler;
	sigemptyset(&newaction.sa_mask);
	newaction.sa_flags = 0;

	sigaction(signum, NULL, origaction);
	sigaction(signum, &newaction, NULL);
}

static void unmask_signal(int signum, struct sigaction *sa)
{
	sigaction(signum, sa, NULL);
}

static FILE *create_tempfile(struct dload_payload *payload, const char *localpath)
{
	int fd;
	FILE *fp;
	char *randpath;
	size_t len;

	/* create a random filename, which is opened with O_EXCL */
	len = strlen(localpath) + 14 + 1;
	MALLOC(randpath, len, RET_ERR(payload->handle, ALPM_ERR_MEMORY, NULL));
	snprintf(randpath, len, "%salpmtmp.XXXXXX", localpath);
	if((fd = mkstemp(randpath)) == -1 ||
			fchmod(fd, ~(_getumask()) & 0666) ||
			!(fp = fdopen(fd, payload->tempfile_openmode))) {
		unlink(randpath);
		close(fd);
		_alpm_log(payload->handle, ALPM_LOG_ERROR,
				_("failed to create temporary file for download\n"));
		free(randpath);
		return NULL;
	}
	/* fp now points to our alpmtmp.XXXXXX */
	free(payload->tempfile_name);
	payload->tempfile_name = randpath;
	free(payload->remote_name);
	STRDUP(payload->remote_name, strrchr(randpath, '/') + 1,
			fclose(fp); RET_ERR(payload->handle, ALPM_ERR_MEMORY, NULL));

	return fp;
}

/* RFC1123 states applications should support this length */
#define HOSTNAME_SIZE 256

static int curl_download_internal(struct dload_payload *payload,
		const char *localpath, char **final_file, const char **final_url)
{
	int ret = -1;
	FILE *localf = NULL;
	char *effective_url;
	char hostname[HOSTNAME_SIZE];
	char error_buffer[CURL_ERROR_SIZE] = {0};
	struct stat st;
	long timecond, remote_time = -1;
	double remote_size, bytes_dl;
	struct sigaction orig_sig_pipe, orig_sig_int;
	CURLcode curlerr;
	alpm_download_event_init_t init_cb_data = {0};
	alpm_download_event_completed_t completed_cb_data = {0};
	/* shortcut to our handle within the payload */
	alpm_handle_t *handle = payload->handle;
	CURL *curl = curl_easy_init();
	handle->pm_errno = ALPM_ERR_OK;
	payload->curl = curl;

	/* make sure these are NULL */
	FREE(payload->tempfile_name);
	FREE(payload->destfile_name);
	FREE(payload->content_disp_name);

	payload->tempfile_openmode = "wb";
	if(!payload->remote_name) {
		STRDUP(payload->remote_name, get_filename(payload->fileurl),
				RET_ERR(handle, ALPM_ERR_MEMORY, -1));
	}
	if(curl_gethost(payload->fileurl, hostname, sizeof(hostname)) != 0) {
		_alpm_log(handle, ALPM_LOG_ERROR, _("url '%s' is invalid\n"), payload->fileurl);
		RET_ERR(handle, ALPM_ERR_SERVER_BAD_URL, -1);
	}

	if(payload->remote_name && strlen(payload->remote_name) > 0 &&
			strcmp(payload->remote_name, ".sig") != 0) {
		payload->destfile_name = get_fullpath(localpath, payload->remote_name, "");
		payload->tempfile_name = get_fullpath(localpath, payload->remote_name, ".part");
		if(!payload->destfile_name || !payload->tempfile_name) {
			goto cleanup;
		}
	} else {
		/* URL doesn't contain a filename, so make a tempfile. We can't support
		 * resuming this kind of download; partial transfers will be destroyed */
		payload->unlink_on_fail = 1;

		localf = create_tempfile(payload, localpath);
		if(localf == NULL) {
			goto cleanup;
		}
	}

	curl_set_handle_opts(payload, curl, error_buffer);

	if(payload->max_size == payload->initial_size && payload->max_size != 0) {
		/* .part file is complete */
		ret = 0;
		goto cleanup;
	}

	if(localf == NULL) {
		localf = fopen(payload->tempfile_name, payload->tempfile_openmode);
		if(localf == NULL) {
			_alpm_log(handle, ALPM_LOG_ERROR,
					_("could not open file %s: %s\n"),
					payload->tempfile_name, strerror(errno));
			GOTO_ERR(handle, ALPM_ERR_RETRIEVE, cleanup);
		}
	}

	_alpm_log(handle, ALPM_LOG_DEBUG,
			"opened tempfile for download: %s (%s)\n", payload->tempfile_name,
			payload->tempfile_openmode);

	curl_easy_setopt(curl, CURLOPT_WRITEDATA, localf);

	/* Ignore any SIGPIPE signals. With libcurl, these shouldn't be happening,
	 * but better safe than sorry. Store the old signal handler first. */
	mask_signal(SIGPIPE, SIG_IGN, &orig_sig_pipe);
	dload_interrupted = 0;
	mask_signal(SIGINT, &inthandler, &orig_sig_int);

	handle->dlcb(payload->remote_name, ALPM_DOWNLOAD_INIT, &init_cb_data);

	/* perform transfer */
	curlerr = curl_easy_perform(curl);
	_alpm_log(handle, ALPM_LOG_DEBUG, "curl returned error %d from transfer\n",
			curlerr);

	/* disconnect relationships from the curl handle for things that might go out
	 * of scope, but could still be touched on connection teardown. This really
	 * only applies to FTP transfers. */
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, (char *)NULL);

	/* was it a success? */
	switch(curlerr) {
		case CURLE_OK:
			/* get http/ftp response code */
			_alpm_log(handle, ALPM_LOG_DEBUG, "response code: %ld\n", payload->respcode);
			if(payload->respcode >= 400) {
				payload->unlink_on_fail = 1;
				if(!payload->errors_ok) {
					handle->pm_errno = ALPM_ERR_RETRIEVE;
					/* non-translated message is same as libcurl */
					snprintf(error_buffer, sizeof(error_buffer),
							"The requested URL returned error: %ld", payload->respcode);
					_alpm_log(handle, ALPM_LOG_ERROR,
							_("failed retrieving file '%s' from %s : %s\n"),
							payload->remote_name, hostname, error_buffer);
				}
				goto cleanup;
			}
			break;
		case CURLE_ABORTED_BY_CALLBACK:
			/* handle the interrupt accordingly */
			if(dload_interrupted == ABORT_OVER_MAXFILESIZE) {
				curlerr = CURLE_FILESIZE_EXCEEDED;
				payload->unlink_on_fail = 1;
				handle->pm_errno = ALPM_ERR_LIBCURL;
				_alpm_log(handle, ALPM_LOG_ERROR,
						_("failed retrieving file '%s' from %s : expected download size exceeded\n"),
						payload->remote_name, hostname);
			}
			goto cleanup;
		case CURLE_COULDNT_RESOLVE_HOST:
			payload->unlink_on_fail = 1;
			handle->pm_errno = ALPM_ERR_SERVER_BAD_URL;
			_alpm_log(handle, ALPM_LOG_ERROR,
					_("failed retrieving file '%s' from %s : %s\n"),
					payload->remote_name, hostname, error_buffer);
			goto cleanup;
		default:
			/* delete zero length downloads */
			if(fstat(fileno(localf), &st) == 0 && st.st_size == 0) {
				payload->unlink_on_fail = 1;
			}
			if(!payload->errors_ok) {
				handle->pm_errno = ALPM_ERR_LIBCURL;
				_alpm_log(handle, ALPM_LOG_ERROR,
						_("failed retrieving file '%s' from %s : %s\n"),
						payload->remote_name, hostname, error_buffer);
			} else {
				_alpm_log(handle, ALPM_LOG_DEBUG,
						"failed retrieving file '%s' from %s : %s\n",
						payload->remote_name, hostname, error_buffer);
			}
			goto cleanup;
	}

	/* retrieve info about the state of the transfer */
	curl_easy_getinfo(curl, CURLINFO_FILETIME, &remote_time);
	curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &remote_size);
	curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &bytes_dl);
	curl_easy_getinfo(curl, CURLINFO_CONDITION_UNMET, &timecond);
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

	if(final_url != NULL) {
		*final_url = effective_url;
	}

	/* time condition was met and we didn't download anything. we need to
	 * clean up the 0 byte .part file that's left behind. */
	if(timecond == 1 && DOUBLE_EQ(bytes_dl, 0)) {
		_alpm_log(handle, ALPM_LOG_DEBUG, "file met time condition\n");
		ret = 1;
		unlink(payload->tempfile_name);
		goto cleanup;
	}

	/* remote_size isn't necessarily the full size of the file, just what the
	 * server reported as remaining to download. compare it to what curl reported
	 * as actually being transferred during curl_easy_perform() */
	if(!DOUBLE_EQ(remote_size, -1) && !DOUBLE_EQ(bytes_dl, -1) &&
			!DOUBLE_EQ(bytes_dl, remote_size)) {
		_alpm_log(handle, ALPM_LOG_ERROR, _("%s appears to be truncated: %jd/%jd bytes\n"),
				payload->remote_name, (intmax_t)bytes_dl, (intmax_t)remote_size);
		GOTO_ERR(handle, ALPM_ERR_RETRIEVE, cleanup);
	}

	if(payload->trust_remote_name) {
		if(payload->content_disp_name) {
			/* content-disposition header has a better name for our file */
			free(payload->destfile_name);
			payload->destfile_name = get_fullpath(localpath,
				get_filename(payload->content_disp_name), "");
		} else {
			const char *effective_filename = strrchr(effective_url, '/');
			if(effective_filename && strlen(effective_filename) > 2) {
				effective_filename++;

				/* if destfile was never set, we wrote to a tempfile. even if destfile is
				 * set, we may have followed some redirects and the effective url may
				 * have a better suggestion as to what to name our file. in either case,
				 * refactor destfile to this newly derived name. */
				if(!payload->destfile_name || strcmp(effective_filename,
							strrchr(payload->destfile_name, '/') + 1) != 0) {
					free(payload->destfile_name);
					payload->destfile_name = get_fullpath(localpath, effective_filename, "");
				}
			}
		}
	}

	ret = 0;

cleanup:
	if(localf != NULL) {
		fclose(localf);
		utimes_long(payload->tempfile_name, remote_time);
	}

	if(ret == 0) {
		const char *realname = payload->tempfile_name;
		if(payload->destfile_name) {
			realname = payload->destfile_name;
			if(rename(payload->tempfile_name, payload->destfile_name)) {
				_alpm_log(handle, ALPM_LOG_ERROR, _("could not rename %s to %s (%s)\n"),
						payload->tempfile_name, payload->destfile_name, strerror(errno));
				ret = -1;
			}
		}
		if(ret != -1 && final_file) {
			STRDUP(*final_file, strrchr(realname, '/') + 1,
					RET_ERR(handle, ALPM_ERR_MEMORY, -1));
		}
	}

	if((ret == -1 || dload_interrupted) && payload->unlink_on_fail &&
			payload->tempfile_name) {
		unlink(payload->tempfile_name);
	}

	curl_easy_cleanup(curl);
	payload->curl = NULL;

	/* restore the old signal handlers */
	unmask_signal(SIGINT, &orig_sig_int);
	unmask_signal(SIGPIPE, &orig_sig_pipe);
	/* if we were interrupted, trip the old handler */
	if(dload_interrupted == ABORT_SIGINT) {
		raise(SIGINT);
	}

	completed_cb_data.total = bytes_dl;
	completed_cb_data.result = ret;
	handle->dlcb(payload->remote_name, ALPM_DOWNLOAD_COMPLETED, &completed_cb_data);

	return ret;
}

/* Return 0 if retry was successful, -1 otherwise */
static int curl_multi_retry_next_server(CURLM *curlm, CURL *curl, struct dload_payload *payload)
{
	const char *server;
	size_t len;
	alpm_handle_t *handle = payload->handle;

	payload->servers = payload->servers->next;
	if(!payload->servers) {
		_alpm_log(payload->handle, ALPM_LOG_DEBUG,
				"%s: no more servers to retry\n", payload->remote_name);
		return -1;
	}
	server = payload->servers->data;

	/* regenerate a new fileurl */
	free(payload->fileurl);
	len = strlen(server) + strlen(payload->filepath) + 2;
	MALLOC(payload->fileurl, len, RET_ERR(handle, ALPM_ERR_MEMORY, -1));
	snprintf(payload->fileurl, len, "%s/%s", server, payload->filepath);

	if(payload->unlink_on_fail) {
		/* we keep the file for a new retry but remove its data if any */
		fflush(payload->localf);
		if(ftruncate(fileno(payload->localf), 0)) {
			RET_ERR(handle, ALPM_ERR_SYSTEM, -1);
		}
		fseek(payload->localf, 0, SEEK_SET);
	}

	/* Set curl with the new URL */
	curl_easy_setopt(curl, CURLOPT_URL, payload->fileurl);

	curl_multi_remove_handle(curlm, curl);
	curl_multi_add_handle(curlm, curl);

	return 0;
}

/* Returns 2 if download retry happened
 * Returns 1 if the file is up-to-date
 * Returns 0 if current payload is completed successfully
 * Returns -1 if an error happened for a required file
 * Returns -2 if an error happened for an optional file
 */
static int curl_multi_check_finished_download(CURLM *curlm, CURLMsg *msg,
		const char *localpath)
{
	alpm_handle_t *handle = NULL;
	struct dload_payload *payload = NULL;
	CURL *curl = msg->easy_handle;
	CURLcode curlerr;
	char *effective_url;
	long timecond;
	double remote_size, bytes_dl = 0;
	long remote_time = -1;
	struct stat st;
	char hostname[HOSTNAME_SIZE];
	int ret = -1;

	curlerr = curl_easy_getinfo(curl, CURLINFO_PRIVATE, &payload);
	ASSERT(curlerr == CURLE_OK, RET_ERR(handle, ALPM_ERR_LIBCURL, -1));
	handle = payload->handle;

	curl_gethost(payload->fileurl, hostname, sizeof(hostname));
	curlerr = msg->data.result;
	_alpm_log(handle, ALPM_LOG_DEBUG, "%s: curl returned result %d from transfer\n",
			payload->remote_name, curlerr);

	/* was it a success? */
	switch(curlerr) {
		case CURLE_OK:
			/* get http/ftp response code */
			_alpm_log(handle, ALPM_LOG_DEBUG, "%s: response code %ld\n",
					payload->remote_name, payload->respcode);
			if(payload->respcode >= 400) {
				payload->unlink_on_fail = 1;
				if(!payload->errors_ok) {
					handle->pm_errno = ALPM_ERR_RETRIEVE;
					/* non-translated message is same as libcurl */
					snprintf(payload->error_buffer, sizeof(payload->error_buffer),
							"The requested URL returned error: %ld", payload->respcode);
					_alpm_log(handle, ALPM_LOG_ERROR,
							_("failed retrieving file '%s' from %s : %s\n"),
							payload->remote_name, hostname, payload->error_buffer);
				}
				if(curl_multi_retry_next_server(curlm, curl, payload) == 0) {
					return 2;
				} else {
					goto cleanup;
				}
			}
			break;
		case CURLE_ABORTED_BY_CALLBACK:
			/* handle the interrupt accordingly */
			if(dload_interrupted == ABORT_OVER_MAXFILESIZE) {
				curlerr = CURLE_FILESIZE_EXCEEDED;
				payload->unlink_on_fail = 1;
				handle->pm_errno = ALPM_ERR_LIBCURL;
				_alpm_log(handle, ALPM_LOG_ERROR,
						_("failed retrieving file '%s' from %s : expected download size exceeded\n"),
						payload->remote_name, hostname);
			}
			goto cleanup;
		case CURLE_COULDNT_RESOLVE_HOST:
			payload->unlink_on_fail = 1;
			handle->pm_errno = ALPM_ERR_SERVER_BAD_URL;
			_alpm_log(handle, ALPM_LOG_ERROR,
					_("failed retrieving file '%s' from %s : %s\n"),
					payload->remote_name, hostname, payload->error_buffer);
			if(curl_multi_retry_next_server(curlm, curl, payload) == 0) {
				return 2;
			} else {
				goto cleanup;
			}
		default:
			/* delete zero length downloads */
			if(fstat(fileno(payload->localf), &st) == 0 && st.st_size == 0) {
				payload->unlink_on_fail = 1;
			}
			if(!payload->errors_ok) {
				handle->pm_errno = ALPM_ERR_LIBCURL;
				_alpm_log(handle, ALPM_LOG_ERROR,
						_("failed retrieving file '%s' from %s : %s\n"),
						payload->remote_name, hostname, payload->error_buffer);
			} else {
				_alpm_log(handle, ALPM_LOG_DEBUG,
						"failed retrieving file '%s' from %s : %s\n",
						payload->remote_name, hostname, payload->error_buffer);
			}
			if(curl_multi_retry_next_server(curlm, curl, payload) == 0) {
				return 2;
			} else {
				goto cleanup;
			}
	}

	/* retrieve info about the state of the transfer */
	curl_easy_getinfo(curl, CURLINFO_FILETIME, &remote_time);
	curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &remote_size);
	curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &bytes_dl);
	curl_easy_getinfo(curl, CURLINFO_CONDITION_UNMET, &timecond);
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

	/* time condition was met and we didn't download anything. we need to
	 * clean up the 0 byte .part file that's left behind. */
	if(timecond == 1 && DOUBLE_EQ(bytes_dl, 0)) {
		_alpm_log(handle, ALPM_LOG_DEBUG, "%s: file met time condition\n",
			payload->remote_name);
		ret = 1;
		unlink(payload->tempfile_name);
		goto cleanup;
	}

	/* remote_size isn't necessarily the full size of the file, just what the
	 * server reported as remaining to download. compare it to what curl reported
	 * as actually being transferred during curl_easy_perform() */
	if(!DOUBLE_EQ(remote_size, -1) && !DOUBLE_EQ(bytes_dl, -1) &&
			!DOUBLE_EQ(bytes_dl, remote_size)) {
		_alpm_log(handle, ALPM_LOG_ERROR, _("%s appears to be truncated: %jd/%jd bytes\n"),
				payload->remote_name, (intmax_t)bytes_dl, (intmax_t)remote_size);
		GOTO_ERR(handle, ALPM_ERR_RETRIEVE, cleanup);
	}

	if(payload->trust_remote_name) {
		if(payload->content_disp_name) {
			/* content-disposition header has a better name for our file */
			free(payload->destfile_name);
			payload->destfile_name = get_fullpath(localpath,
				get_filename(payload->content_disp_name), "");
		} else {
			const char *effective_filename = strrchr(effective_url, '/');
			if(effective_filename && strlen(effective_filename) > 2) {
				effective_filename++;

				/* if destfile was never set, we wrote to a tempfile. even if destfile is
				 * set, we may have followed some redirects and the effective url may
				 * have a better suggestion as to what to name our file. in either case,
				 * refactor destfile to this newly derived name. */
				if(!payload->destfile_name || strcmp(effective_filename,
							strrchr(payload->destfile_name, '/') + 1) != 0) {
					free(payload->destfile_name);
					payload->destfile_name = get_fullpath(localpath, effective_filename, "");
				}
			}
		}
	}

	ret = 0;

cleanup:
	/* disconnect relationships from the curl handle for things that might go out
	 * of scope, but could still be touched on connection teardown. This really
	 * only applies to FTP transfers. */
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, (char *)NULL);

	if(payload->localf != NULL) {
		fclose(payload->localf);
		utimes_long(payload->tempfile_name, remote_time);
	}

	if(ret == 0) {
		if(payload->destfile_name) {
			if(rename(payload->tempfile_name, payload->destfile_name)) {
				_alpm_log(handle, ALPM_LOG_ERROR, _("could not rename %s to %s (%s)\n"),
						payload->tempfile_name, payload->destfile_name, strerror(errno));
				ret = -1;
			}
		}
	}

	if((ret == -1 || dload_interrupted) && payload->unlink_on_fail &&
			payload->tempfile_name) {
		unlink(payload->tempfile_name);
	}

	if(!payload->signature) {
		alpm_download_event_completed_t cb_data = {0};
		cb_data.total = bytes_dl;
		cb_data.result = ret;
		handle->dlcb(payload->remote_name, ALPM_DOWNLOAD_COMPLETED, &cb_data);
	}

	curl_multi_remove_handle(curlm, curl);
	curl_easy_cleanup(curl);
	payload->curl = NULL;

	FREE(payload->fileurl);

	if(ret == -1 && payload->errors_ok) {
		ret = -2;
	}
	return ret;
}

/* Returns 0 in case if a new download transaction has been successfully started
 * Returns -1 if am error happened while starting a new download
 */
static int curl_multi_add_payload(alpm_handle_t *handle, CURLM *curlm,
		struct dload_payload *payload, const char *localpath)
{
	size_t len;
	const char *server;
	CURL *curl = NULL;
	char hostname[HOSTNAME_SIZE];
	int ret = -1;

	ASSERT(payload->servers, RET_ERR(handle, ALPM_ERR_SERVER_NONE, -1));
	server = payload->servers->data;

	curl = curl_easy_init();
	payload->curl = curl;

	len = strlen(server) + strlen(payload->filepath) + 2;
	MALLOC(payload->fileurl, len, GOTO_ERR(handle, ALPM_ERR_MEMORY, cleanup));
	snprintf(payload->fileurl, len, "%s/%s", server, payload->filepath);

	payload->tempfile_openmode = "wb";
	if(!payload->remote_name) {
		STRDUP(payload->remote_name, get_filename(payload->fileurl),
			GOTO_ERR(handle, ALPM_ERR_MEMORY, cleanup));
	}
	if(curl_gethost(payload->fileurl, hostname, sizeof(hostname)) != 0) {
		_alpm_log(handle, ALPM_LOG_ERROR, _("url '%s' is invalid\n"), payload->fileurl);
		GOTO_ERR(handle, ALPM_ERR_SERVER_BAD_URL, cleanup);
	}

	if(payload->remote_name && strlen(payload->remote_name) > 0) {
		payload->destfile_name = get_fullpath(localpath, payload->remote_name, "");
		payload->tempfile_name = get_fullpath(localpath, payload->remote_name, ".part");
		if(!payload->destfile_name || !payload->tempfile_name) {
			goto cleanup;
		}
	} else {
		/* URL doesn't contain a filename, so make a tempfile. We can't support
		 * resuming this kind of download; partial transfers will be destroyed */
		payload->unlink_on_fail = 1;

		payload->localf = create_tempfile(payload, localpath);
		if(payload->localf == NULL) {
			goto cleanup;
		}
	}

	curl_set_handle_opts(payload, curl, payload->error_buffer);

	if(payload->max_size == payload->initial_size && payload->max_size != 0) {
		/* .part file is complete */
		ret = 0;
		goto cleanup;
	}

	if(payload->localf == NULL) {
		payload->localf = fopen(payload->tempfile_name, payload->tempfile_openmode);
		if(payload->localf == NULL) {
			_alpm_log(handle, ALPM_LOG_ERROR,
					_("could not open file %s: %s\n"),
					payload->tempfile_name, strerror(errno));
			GOTO_ERR(handle, ALPM_ERR_RETRIEVE, cleanup);
		}
	}

	_alpm_log(handle, ALPM_LOG_DEBUG,
			"%s: opened tempfile for download: %s (%s)\n",
			payload->remote_name,
			payload->tempfile_name,
			payload->tempfile_openmode);

	curl_easy_setopt(curl, CURLOPT_WRITEDATA, payload->localf);
	curl_multi_add_handle(curlm, curl);
	return 0;

cleanup:
	FREE(payload->fileurl);
	FREE(payload->tempfile_name);
	FREE(payload->destfile_name);
	FREE(payload->content_disp_name);
	curl_easy_cleanup(curl);
	return ret;
}

static int curl_multi_download_internal(alpm_handle_t *handle,
		alpm_list_t *payloads /* struct dload_payload */,
		const char *localpath)
{
	int still_running = 0;
	int err = 0;
	int parallel_downloads = handle->parallel_downloads;

	CURLM *curlm = handle->curlm;
	CURLMsg *msg;

	while(still_running || payloads) {
		int msgs_left = -1;

		for(; still_running < parallel_downloads && payloads; still_running++) {
			struct dload_payload *payload = payloads->data;

			if(curl_multi_add_payload(handle, curlm, payload, localpath) == 0) {
				if(!payload->signature) {
					alpm_download_event_init_t cb_data = {.optional = payload->errors_ok};
					handle->dlcb(payload->remote_name, ALPM_DOWNLOAD_INIT, &cb_data);
				}

				payloads = payloads->next;
			} else {
				// the payload failed to start, do not start any new downloads just wait until
				// active one complete.
				_alpm_log(handle, ALPM_LOG_ERROR, _("failed to setup a download payload for %s\n"), payload->remote_name);
				payloads = NULL;
				err = -1;
			}
		}

		CURLMcode mc = curl_multi_perform(curlm, &still_running);

		if(mc != CURLM_OK) {
			_alpm_log(handle, ALPM_LOG_ERROR, _("curl returned error %d from transfer\n"), mc);
			payloads = NULL;
			err = -1;
		}

		while((msg = curl_multi_info_read(curlm, &msgs_left))) {
			if(msg->msg == CURLMSG_DONE) {
				int ret = curl_multi_check_finished_download(curlm, msg, localpath);
				if(ret == -1) {
					/* if current payload failed to download then stop adding new payloads but wait for the
					 * current ones
					 */
					payloads = NULL;
					err = -1;
				} else if(ret == 2) {
					/* in case of a retry increase the counter of active requests
					 * to avoid exiting the loop early
					 */
					still_running++;
				}
			} else {
				_alpm_log(handle, ALPM_LOG_ERROR, _("curl transfer error: %d\n"), msg->msg);
			}
		}
		if(still_running) {
			curl_multi_wait(curlm, NULL, 0, 1000, NULL);
		}
	}

	_alpm_log(handle, ALPM_LOG_DEBUG, "curl_multi_download_internal return code is %d\n", err);
	return err;
}

#endif

/** Download a file given by a URL to a local directory.
 * Does not overwrite an existing file if the download fails.
 * @param payload the payload context
 * @param localpath the directory to save the file in
 * @param final_file the real name of the downloaded file (may be NULL)
 * @return 0 on success, -1 on error (pm_errno is set accordingly if errors_ok == 0)
 */
int _alpm_download(struct dload_payload *payload, const char *localpath,
		char **final_file, const char **final_url)
{
	alpm_handle_t *handle = payload->handle;

	if(handle->fetchcb == NULL) {
#ifdef HAVE_LIBCURL
		return curl_download_internal(payload, localpath, final_file, final_url);
#else
		/* work around unused warnings when building without libcurl */
		(void)final_file;
		(void)final_url;
		RET_ERR(handle, ALPM_ERR_EXTERNAL_DOWNLOAD, -1);
#endif
	} else {
		int ret = handle->fetchcb(payload->fileurl, localpath, payload->force);
		if(ret == -1 && !payload->errors_ok) {
			RET_ERR(handle, ALPM_ERR_EXTERNAL_DOWNLOAD, -1);
		}
		return ret;
	}
}

int _alpm_multi_download(alpm_handle_t *handle,
		alpm_list_t *payloads /* struct dload_payload */,
		const char *localpath)
{
	if(handle->fetchcb == NULL) {
#ifdef HAVE_LIBCURL
		return curl_multi_download_internal(handle, payloads, localpath);
#else
		RET_ERR(handle, ALPM_ERR_EXTERNAL_DOWNLOAD, -1);
#endif
	} else {
		alpm_list_t *p;
		for(p = payloads; p; p = p->next) {
			struct dload_payload *payload = p->data;
			alpm_list_t *s;
			int success = 0;

			for(s = payload->servers; s; s = s->next) {
				const char *server = s->data;
				char *fileurl;
				int ret;

				size_t len = strlen(server) + strlen(payload->filepath) + 2;
				MALLOC(fileurl, len, RET_ERR(handle, ALPM_ERR_MEMORY, -1));
				snprintf(fileurl, len, "%s/%s", server, payload->filepath);

				ret = handle->fetchcb(fileurl, localpath, payload->force);
				free(fileurl);

				if (ret != -1) {
					success = 1;
					break;
				}
			}
			if(!success && !payload->errors_ok) {
				RET_ERR(handle, ALPM_ERR_EXTERNAL_DOWNLOAD, -1);
			}
		}
		return 0;
	}
}

static char *filecache_find_url(alpm_handle_t *handle, const char *url)
{
	const char *filebase = strrchr(url, '/');

	if(filebase == NULL) {
		return NULL;
	}

	filebase++;
	if(*filebase == '\0') {
		return NULL;
	}

	return _alpm_filecache_find(handle, filebase);
}

char SYMEXPORT *alpm_fetch_pkgurl(alpm_handle_t *handle, const char *url)
{
	char *filepath;
	const char *cachedir, *final_pkg_url = NULL;
	char *final_file = NULL;
	struct dload_payload payload = {0};
	int ret = 0;

	CHECK_HANDLE(handle, return NULL);
	ASSERT(url, RET_ERR(handle, ALPM_ERR_WRONG_ARGS, NULL));

	/* find a valid cache dir to download to */
	cachedir = _alpm_filecache_setup(handle);

	/* attempt to find the file in our pkgcache */
	filepath = filecache_find_url(handle, url);
	if(filepath == NULL) {
		STRDUP(payload.fileurl, url, RET_ERR(handle, ALPM_ERR_MEMORY, NULL));
		payload.allow_resume = 1;
		payload.handle = handle;
		payload.trust_remote_name = 1;

		/* download the file */
		ret = _alpm_download(&payload, cachedir, &final_file, &final_pkg_url);
		_alpm_dload_payload_reset(&payload);
		if(ret == -1) {
			_alpm_log(handle, ALPM_LOG_WARNING, _("failed to download %s\n"), url);
			free(final_file);
			return NULL;
		}
		_alpm_log(handle, ALPM_LOG_DEBUG, "successfully downloaded %s\n", url);
	}

	/* attempt to download the signature */
	if(ret == 0 && final_pkg_url && (handle->siglevel & ALPM_SIG_PACKAGE)) {
		char *sig_filepath, *sig_final_file = NULL;
		size_t len;

		len = strlen(final_pkg_url) + 5;
		MALLOC(payload.fileurl, len, free(final_file); RET_ERR(handle, ALPM_ERR_MEMORY, NULL));
		snprintf(payload.fileurl, len, "%s.sig", final_pkg_url);

		sig_filepath = filecache_find_url(handle, payload.fileurl);
		if(sig_filepath == NULL) {
			payload.signature = 1;
			payload.handle = handle;
			payload.trust_remote_name = 1;
			payload.force = 1;
			payload.errors_ok = (handle->siglevel & ALPM_SIG_PACKAGE_OPTIONAL);

			/* set hard upper limit of 16KiB */
			payload.max_size = 16 * 1024;

			ret = _alpm_download(&payload, cachedir, &sig_final_file, NULL);
			if(ret == -1 && !payload.errors_ok) {
				_alpm_log(handle, ALPM_LOG_WARNING,
						_("failed to download %s\n"), payload.fileurl);
				/* Warn now, but don't return NULL. We will fail later during package
				 * load time. */
			} else if(ret == 0) {
				_alpm_log(handle, ALPM_LOG_DEBUG,
						"successfully downloaded %s\n", payload.fileurl);
			}
			FREE(sig_final_file);
		}
		free(sig_filepath);
		_alpm_dload_payload_reset(&payload);
	}

	/* we should be able to find the file the second time around */
	if(filepath == NULL) {
		filepath = _alpm_filecache_find(handle, final_file);
	}
	free(final_file);

	return filepath;
}

void _alpm_dload_payload_reset(struct dload_payload *payload)
{
	ASSERT(payload, return);

	FREE(payload->remote_name);
	FREE(payload->tempfile_name);
	FREE(payload->destfile_name);
	FREE(payload->content_disp_name);
	FREE(payload->fileurl);
	FREE(payload->filepath);
	*payload = (struct dload_payload){0};
}

void _alpm_dload_payload_reset_for_retry(struct dload_payload *payload)
{
	ASSERT(payload, return);

	FREE(payload->fileurl);
	FREE(payload->filepath);
	payload->initial_size += payload->prevprogress;
	payload->prevprogress = 0;
	payload->unlink_on_fail = 0;
}
