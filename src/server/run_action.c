#include "include.h"
#include "burp1/backup.h"
#include "burp1/rubble.h"
#include "burp2/backup.h"
#include "burp2/restore.h"
#include "burp2/rubble.h"

// FIX THIS: Somewhat haphazard.
/* Return 0 for everything OK. -1 for error, or 1 to mean that a backup is
   currently finalising. */
static int get_lock_sdirs(struct asfd *asfd, struct sdirs *sdirs)
{
	struct stat statp;

	// Make sure the lock directory exists.
	if(mkpath(&sdirs->lock->path, sdirs->lockdir))
	{
		asfd->write_str(asfd, CMD_ERROR, "problem with lock directory");
		goto error;
	}

	lock_get(sdirs->lock);
	switch(sdirs->lock->status)
	{
		case GET_LOCK_GOT: break;
		case GET_LOCK_NOT_GOT:
			logp("Another instance of client is already running.\n");
			asfd->write_str(asfd, CMD_ERROR,
				"another instance is already running");
			goto error;
		case GET_LOCK_ERROR:
		default:
			logp("Problem with lock file on server: %s\n",
				sdirs->lock->path);
			asfd->write_str(asfd, CMD_ERROR,
				"problem with lock file on server");
			goto error;
	}

	if(!lstat(sdirs->finishing, &statp))
	{
		char msg[256]="";
		logp("finalising previous backup\n");
		snprintf(msg, sizeof(msg),
			"Finalising previous backup of client. "
			"Please try again later.");
		asfd->write_str(asfd, CMD_ERROR, msg);
		goto finalising;
	}

	return 0;
finalising:
	return 1;
error:
	return -1;
}

static int client_can_restore(struct conf *cconf)
{
	struct stat statp;
	// If there is a restore file on the server, it is always OK.
	if(cconf->restore_path
	  && !lstat(cconf->restore_path, &statp))
	{
		// Remove the file.
		unlink(cconf->restore_path);
		return 1;
	}
	return cconf->client_can_restore;
}

static void maybe_do_notification(struct asfd *asfd,
	int status, const char *clientdir,
	const char *storagedir, const char *filename,
	const char *brv, struct conf *cconf)
{
	int a=0;
	const char *args[12];
	struct cntr *cntr=cconf->cntr;
	args[a++]=NULL; // Fill in the script name later.
	args[a++]=cconf->cname;
	args[a++]=clientdir;
	args[a++]=storagedir;
	args[a++]=filename;
	args[a++]=brv;
	if(status)
	{
		args[0]=cconf->n_failure_script;
		args[a++]="0";
		args[a++]=NULL;
		run_script(asfd, args, cconf->n_failure_arg, cconf, 1, 1, 1);
	}
	else if((cconf->n_success_warnings_only
		&& cntr->ent[CMD_WARNING]->count > 0)
	  || (cconf->n_success_changes_only
		&& cntr->ent[CMD_TOTAL]->changed > 0)
	  || (!cconf->n_success_warnings_only
		&& !cconf->n_success_changes_only))
	{
		char warnings[32]="";
		snprintf(warnings, sizeof(warnings), "%llu",
			cntr->ent[CMD_WARNING]->count);
		args[0]=cconf->n_success_script;
		args[a++]=warnings;
		args[a++]=NULL;
		run_script(asfd, args, cconf->n_success_arg, cconf, 1, 1, 1);
	}
}

static int run_backup(struct async *as,
	struct sdirs *sdirs, struct conf *cconf,
	struct iobuf *rbuf, const char *incexc, int *timer_ret, int resume)
{
	int ret=-1;
	char okstr[32]="";
	struct asfd *asfd=as->asfd;

	if(cconf->restore_client)
	{
		// This client is not the original client, so a
		// backup might cause all sorts of trouble.
		logp("Not allowing backup of %s\n", cconf->cname);
		return asfd->write_str(asfd, CMD_GEN, "Backup is not allowed");
	}

	// Set quality of service bits on backups.
	asfd->set_bulk_packets(asfd);

	if(!strncmp_w(rbuf->buf, "backupphase1timed"))
	{
		int a=0;
		const char *args[12];
		int checkonly=!strncmp_w(rbuf->buf, "backupphase1timedcheck");
		if(checkonly) logp("Client asked for a timer check only.\n");

		args[a++]=cconf->timer_script;
		args[a++]=cconf->cname;
		args[a++]=sdirs->current;
		args[a++]=sdirs->client;
		args[a++]="reserved1";
		args[a++]="reserved2";
		args[a++]=NULL;
		if((*timer_ret=run_script(asfd, args,
		  cconf->timer_arg,
		  cconf,
		  1 /* wait */, 1 /* use logp */,
		  0 /* no logw so that run_script does not
		       write warnings down the socket, otherwise
		       the client will never print the 'timer
		       conditions not met' message below. */
		))<0)
		{
			logp("Error running timer script for %s\n",
				cconf->cname);
			maybe_do_notification(asfd, ret, "",
				"error running timer script",
				"", "backup", cconf);
			return *timer_ret;
		}
		if(*timer_ret)
		{
			if(!checkonly)
				logp("Not running backup of %s\n",
					cconf->cname);
			return asfd->write_str(asfd,
				CMD_GEN, "timer conditions not met");
		}
		if(checkonly)
		{
			// Client was only checking the timer
			// and does not actually want to back
			// up.
			logp("Client asked for a timer check only,\n");
			logp("so a backup is not happening right now.\n");
			return asfd->write_str(asfd,
				CMD_GEN, "timer conditions met");
		}
		logp("Running backup of %s\n", cconf->cname);
	}
	else if(!cconf->client_can_force_backup)
	{
		logp("Not allowing forced backup of %s\n", cconf->cname);
		return asfd->write_str(asfd,
			CMD_GEN, "Forced backup is not allowed");
	}

	snprintf(okstr, sizeof(okstr), "%s:%d",
		resume?"resume":"ok", cconf->compression);
	if(asfd->write_str(asfd, CMD_GEN, okstr)) return -1;
	if(cconf->protocol==PROTO_BURP1)
		ret=do_backup_server_burp1(as, sdirs, cconf, incexc, resume);
	else
		ret=do_backup_server_burp2(as, sdirs, cconf, incexc, resume);
	maybe_do_notification(asfd, ret, sdirs->client, sdirs->current,
		"log", "backup", cconf);

	return ret;
}

static int run_restore(struct asfd *asfd,
	struct sdirs *sdirs, struct conf *cconf,
	struct iobuf *rbuf, int srestore)
{
	int ret=-1;
	char *cp=NULL;
	char *copy=NULL;
	enum action act;
	char *backupnostr=NULL;
	char *restoreregex=NULL;
	char *dir_for_notify=NULL;

	if(!(copy=strdup_w(rbuf->buf, __func__)))
		goto end;

	iobuf_free_content(rbuf);

	if(!strncmp_w(copy, "restore ")) act=ACTION_RESTORE;
	else act=ACTION_VERIFY;

	if(!(backupnostr=strchr(copy, ' ')))
	{
		logp("Could not parse %s in %s\n", copy, __func__);
		goto end;
	}
	if(conf_val_reset(backupnostr, &(cconf->backup)))
		goto end;
	if((cp=strchr(cconf->backup, ':'))) *cp='\0';

	if(act==ACTION_RESTORE)
	{
		int r;
		if((r=client_can_restore(cconf))<0)
			goto end;
		else if(!r)
		{
			logp("Not allowing restore of %s\n", cconf->cname);
			if(!asfd->write_str(asfd, CMD_GEN,
				"Client restore is not allowed")) ret=0;
			goto end;
		}
	}
	if(act==ACTION_VERIFY && !cconf->client_can_verify)
	{
		logp("Not allowing verify of %s\n", cconf->cname);
		if(!asfd->write_str(asfd, CMD_GEN,
			"Client verify is not allowed")) ret=0;
		goto end;
	}

	if((restoreregex=strchr(copy, ':')))
	{
		*restoreregex='\0';
		restoreregex++;
	}
	if(conf_val_reset(restoreregex, &(cconf->regex))
	  || asfd->write_str(asfd, CMD_GEN, "ok"))
		goto end;
	ret=do_restore_server(asfd, sdirs, act,
		srestore, &dir_for_notify, cconf);
	if(dir_for_notify)
		maybe_do_notification(asfd, ret,
			sdirs->client, dir_for_notify,
			act==ACTION_RESTORE?"restorelog":"verifylog",
			act==ACTION_RESTORE?"restore":"verify",
			cconf);
end:
	free_w(&copy);
	free_w(&dir_for_notify);
	return ret;
}

static int run_delete(struct asfd *asfd,
	struct sdirs *sdirs, struct conf *cconf, struct iobuf *rbuf)
{
	char *backupno=NULL;
	if(!cconf->client_can_delete)
	{
		logp("Not allowing delete of %s\n", cconf->cname);
		asfd->write_str(asfd, CMD_GEN, "Client delete is not allowed");
		return -1;
	}
	backupno=rbuf->buf+strlen("delete ");
	return do_delete_server(asfd, sdirs, cconf, backupno);
}

static int run_list(struct asfd *asfd,
	struct sdirs *sdirs, struct conf *cconf, struct iobuf *rbuf)
{
	char *backupno=NULL;
	char *browsedir=NULL;
	char *listregex=NULL;

	if(!cconf->client_can_list)
	{
		logp("Not allowing list of %s\n", cconf->cname);
		asfd->write_str(asfd, CMD_GEN, "Client list is not allowed");
		return -1;
	}

	if(!strncmp_w(rbuf->buf, "list "))
	{
		if((listregex=strrchr(rbuf->buf, ':')))
		{
			*listregex='\0';
			listregex++;
		}
		backupno=rbuf->buf+strlen("list ");
	}
	else if(!strncmp_w(rbuf->buf, "listb "))
	{
		if((browsedir=strchr(rbuf->buf, ':')))
		{
			*browsedir='\0';
			browsedir++;
		}
		strip_trailing_slashes(&browsedir);
		backupno=rbuf->buf+strlen("listb ");
	}
	if(asfd->write_str(asfd, CMD_GEN, "ok")) return -1;

	return do_list_server(asfd,
		sdirs, cconf, backupno, listregex, browsedir);
}

static int unknown_command(struct asfd *asfd, struct iobuf *rbuf)
{
	iobuf_log_unexpected(rbuf, __func__);
	asfd->write_str(asfd, CMD_ERROR, "unknown command");
	return -1;
}

static const char *buf_to_notify_str(const char *buf)
{
	if(!strncmp_w(buf, "backup")) return "backup";
	else if(!strncmp_w(buf, "restore")) return "restore";
	else if(!strncmp_w(buf, "verify")) return "verify";
	else if(!strncmp_w(buf, "delete")) return "delete";
	else if(!strncmp_w(buf, "list")) return "list";
	else return "unknown";
}

static int check_for_rubble(struct asfd *asfd, struct sdirs *sdirs,
	struct conf *cconf, const char *incexc, int *resume)
{
	if(cconf->protocol==PROTO_BURP1)
		return check_for_rubble_burp1(asfd,
			sdirs, cconf, incexc, resume);
	else
		return check_for_rubble_burp2(asfd,
			sdirs, cconf, incexc, resume);
}

int run_action_server(struct async *as,
	struct conf *cconf, struct sdirs *sdirs,
	const char *incexc, int srestore, int *timer_ret)
{
	int ret;
	int resume=0;
	char msg[256]="";
	struct iobuf *rbuf=as->asfd->rbuf;

	// Make sure some directories exist.
	if(mkpath(&sdirs->current, sdirs->dedup))
	{
		snprintf(msg, sizeof(msg),
			"could not mkpath %s", sdirs->current);
		log_and_send(as->asfd, msg);
		return -1;
	}

	if(rbuf->cmd!=CMD_GEN)
		return unknown_command(as->asfd, rbuf);
	if((ret=get_lock_sdirs(as->asfd, sdirs)))
	{
		// -1 on error or 1 if the backup is still finalising.
		if(ret<0) maybe_do_notification(as->asfd, ret,
			"", "error in get_lock_sdirs()",
			"", buf_to_notify_str(rbuf->buf), cconf);
		return ret;
	}

	if(check_for_rubble(as->asfd, sdirs, cconf, incexc, &resume))
	{
		maybe_do_notification(as->asfd, ret,
			"", "error in check_for_rubble()",
			"", buf_to_notify_str(rbuf->buf), cconf);
		return -1;
	}

	if(!strncmp_w(rbuf->buf, "backup"))
		return run_backup(as, sdirs, cconf,
			rbuf, incexc, timer_ret, resume);

	if(!strncmp_w(rbuf->buf, "restore ")
	  || !strncmp_w(rbuf->buf, "verify "))
		return run_restore(as->asfd, sdirs, cconf, rbuf, srestore);

	if(!strncmp_w(rbuf->buf, "delete "))
		return run_delete(as->asfd, sdirs, cconf, rbuf);

	if(!strncmp_w(rbuf->buf, "list ")
	  || !strncmp_w(rbuf->buf, "listb "))
		return run_list(as->asfd, sdirs, cconf, rbuf);

	return unknown_command(as->asfd, rbuf);
}
