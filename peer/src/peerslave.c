/*
 * Copyright (C) 2010-2011, Robert Oostenveld
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/
 *
 */ 

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/wait.h>

#include "engine.h"
#include "matrix.h"

#include "peer.h"
#include "parser.h"
#include "extern.h"
#include "platform_includes.h"

mxArray *mxSerialize(const mxArray*);
mxArray *mxDeserialize(const void*, size_t);

#define ENGINETIMEOUT     180    /* int, in seconds */
#define ZOMBIETIMEOUT     900    /* int, in seconds */
#define SLEEPTIME         0.010  /* float, in seconds */

#define STARTCMD "matlab -nosplash"

void print_help(char *argv[]) {
		printf("\n");
		printf("This starts a FieldTrip peer-to-peer distributed computing peer, which\n");
		printf("will wait for an incoming job and subsequently start the MATLAB engine and\n");
		printf("evaluate the job. Use as\n");
		printf("  %s [options]\n", argv[0]);
		printf("where the options can include\n");
		printf("  --number      = number, number of slaves to start        (default = 1)\n");
		printf("  --memavail    = number, amount of memory available       (default = inf)\n");
		printf("  --cpuavail    = number, speed of the CPU                 (default = inf)\n");
		printf("  --timavail    = number, maximum duration of a single job (default = inf)\n");
		printf("  --allowhost   = {...}\n");
		printf("  --allowuser   = {...}\n");
		printf("  --allowgroup  = {...}\n");
		printf("  --group       = string\n");
		printf("  --hostname    = string\n");
		printf("  --matlab      = string\n");
		printf("  --timeout     = number, time to keep the engine running after the job finished\n");
		printf("  --smartshare  = 0|1\n");
		printf("  --smartmem    = 0|1\n");
		printf("  --smartcpu    = 0|1\n");
		printf("  --verbose     = number, between 0 and 7 (default = 4)\n");
		printf("  --help\n");
		printf("\n");
}

int help_flag;
int tcpserver_flag = 1;
int udpserver_flag = 0;
int udsserver_flag = 0;

int main(int argc, char *argv[]) {
		Engine *en;
		mxArray *argin, *argout, *options, *arg, *opt;
		joblist_t  *job  = NULL;
		peerlist_t *peer = NULL;
		jobdef_t   *def  = NULL;
		pid_t childpid;

		int matlabRunning = 0, matlabStart, matlabFinished, engineFailed = 0;
		int i, n, c, rc, status, found, handshake, success, server, jobnum = 0, engineAborted = 0, jobFailed = 0, timallow;
		unsigned int enginetimeout = ENGINETIMEOUT;
		unsigned int zombietimeout = ZOMBIETIMEOUT;
		unsigned int peerid, jobid, numpeer;
		char *str = NULL, *startcmd = NULL;

		config_t *cconf, *pconf; /* for the configuration file or command line options */

		userlist_t  *allowuser;
		grouplist_t *allowgroup;
		hostlist_t  *allowhost;

		/* the thread IDs are needed for cancelation at cleanup */
		pthread_t udsserverThread;
		pthread_t tcpserverThread;
		pthread_t announceThread;
		pthread_t discoverThread;
		pthread_t expireThread;

#if SYSLOG==1
		openlog("peerslave", LOG_PID | LOG_PERROR, LOG_USER);
		/* this corresponds to verbose level 4 */
		setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR));
#endif
		peerinit(NULL);

		if (argc==2)
		{
				/* read the options from the configuration file */
				numpeer = parsefile(argv[1], &pconf);
				if (numpeer<1)
						PANIC("cannot read the configuration file");

		}
		else
		{
				pconf = (config_t *)malloc(sizeof(config_t));
				initconfig(pconf);
				numpeer = 1;

				/* use GNU getopt_long for the command-line options */
				while (1)
				{
						static struct option long_options[] =
						{
								{"help",       no_argument, &help_flag, 1},
								{"memavail",   required_argument, 0,  1}, /* numeric argument */
								{"cpuavail",   required_argument, 0,  2}, /* numeric argument */
								{"timavail",   required_argument, 0,  3}, /* numeric argument */
								{"hostname",   required_argument, 0,  4}, /* single string argument */
								{"group",      required_argument, 0,  5}, /* single string argument */
								{"allowuser",  required_argument, 0,  6}, /* single or multiple string argument */
								{"allowhost",  required_argument, 0,  7}, /* single or multiple string argument */
								{"allowgroup", required_argument, 0,  8}, /* single or multiple string argument */
								{"matlab",     required_argument, 0,  9}, /* single string argument */
								{"smartmem",   required_argument, 0, 10}, /* boolean, 0 or 1 */
								{"smartcpu",   required_argument, 0, 11}, /* boolean, 0 or 1 */
								{"smartshare", required_argument, 0, 12}, /* boolean, 0 or 1 */
								{"timeout",    required_argument, 0, 13}, /* numeric argument */
								{"verbose",    required_argument, 0, 14}, /* numeric argument */
								{0, 0, 0, 0}
						};

						/* getopt_long stores the option index here. */
						int option_index = 0;

						c = getopt_long (argc, argv, "", long_options, &option_index); 

						/* detect the end of the options. */
						if (c == -1)
								break;

						switch (c)
						{
								case 0:
										/* if this option set a flag, do nothing else now. */
										if (long_options[option_index].flag != 0)
												break;
										break;

								case 1:
										DEBUG(LOG_NOTICE, "option --memavail with value `%s'", optarg);
										pconf->memavail = optarg;
										break;

								case 2:
										DEBUG(LOG_NOTICE, "option --cpuavail with value `%s'", optarg);
										pconf->cpuavail = optarg;
										break;

								case 3:
										DEBUG(LOG_NOTICE, "option --timavail with value `%s'", optarg);
										pconf->timavail = optarg;
										break;

								case 4:
										DEBUG(LOG_NOTICE, "option --hostname with value `%s'", optarg);
										pconf->hostname = optarg;
										break;

								case 5:
										DEBUG(LOG_NOTICE, "option --group with value `%s'", optarg);
										pconf->group = optarg;
										break;

								case 6:
										DEBUG(LOG_NOTICE, "option --allowuser with value `%s'", optarg);
										pconf->allowuser = optarg;
										break;

								case 7:
										DEBUG(LOG_NOTICE, "option --allowhost with value `%s'", optarg);
										pconf->allowhost = optarg;
										break;

								case 8:
										DEBUG(LOG_NOTICE, "option --allowgroup with value `%s'", optarg);
										pconf->allowgroup = optarg;
										break;

								case 9:
										DEBUG(LOG_NOTICE, "option --matlab with value `%s'", optarg);
										pconf->matlab = optarg;
										break;

								case 10:
										DEBUG(LOG_NOTICE, "option --smartmem with value `%s'", optarg);
										pconf->smartmem = optarg;
										break;

								case 11:
										DEBUG(LOG_NOTICE, "option --smartcpu with value `%s'", optarg);
										pconf->smartcpu = optarg;
										break;

								case 12:
										DEBUG(LOG_NOTICE, "option --smartshare with value `%s'", optarg);
										pconf->smartshare = optarg;
										break;

								case 13:
										DEBUG(LOG_NOTICE, "option --timeout with value `%s'", optarg);
										pconf->timeout = optarg;
										break;

								case 14:
										DEBUG(LOG_NOTICE, "option --verbose with value `%s'", optarg);
										pconf->verbose = optarg;
										break;

								default:
										PANIC("invalid command line options\n");
										break;
						}
				} /* while(1) for getopt */
		} /* if config file or getopt */

		if (help_flag) {
				/* display the help message and return to the command line */
				print_help(argv);
				exit(0);
		}

		/* although the configuration file allows setting the verbose for each peer */
		/* the first ocurrence determines what will be used the parent and all children */
		if (pconf->verbose)
		{
				syslog_level = atol(pconf->verbose);
#if SYSLOG == 1
				switch (syslog_level) {
						case 0:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR) | LOG_MASK(LOG_WARNING) | LOG_MASK(LOG_NOTICE) | LOG_MASK(LOG_INFO) | LOG_MASK(LOG_DEBUG));
								break;
						case 1:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR) | LOG_MASK(LOG_WARNING) | LOG_MASK(LOG_NOTICE) | LOG_MASK(LOG_INFO));
								break;
						case 2:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR) | LOG_MASK(LOG_WARNING) | LOG_MASK(LOG_NOTICE));
								break;
						case 3:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR) | LOG_MASK(LOG_WARNING));
								break;
						case 4:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR));
								break;
						case 5:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT));
								break;
						case 6:
								setlogmask(LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT));
								break;
						case 7:
								setlogmask(LOG_MASK(LOG_EMERG));
								break;
				}
#endif
		}

		/*************************************************************************************
		 * the following section deals with starting multiple peerslaves
		 * and ensuring that they are restarted if any of them exits  
		 ************************************************************************************/

#ifndef WIN32
		DEBUG(LOG_EMERG, "need to start %d children", numpeer);
		cconf = pconf;
		while (1) {

				if (cconf->pid) {
						/* check the status of the child process */
						if (waitpid(cconf->pid, &status, WNOHANG)>0) {
								if (WIFEXITED(status)) {
										DEBUG(LOG_CRIT, "child %d exited", cconf->pid);
										cconf->pid = 0; /* this will cause it to be restarted */
								}
								if (WIFSIGNALED(status)) {
										DEBUG(LOG_CRIT, "child %d signaled", cconf->pid);
										cconf->pid = 0; /* this will cause it to be restarted */
								}
								if (WIFSTOPPED(status)) {
										DEBUG(LOG_CRIT, "child %d stopped", cconf->pid);
								}
						}
				} /* if pid!=0 */

				if (cconf->pid==0) {
						/* start or restart the child process */

						/* increment the host id, this should be unique */
						pthread_mutex_lock(&mutexhost);
						host->id++;
						pthread_mutex_unlock(&mutexhost);

						/* create a new child process */
						childpid = fork();

						/* fork() returns 0 to the child process */
						/* fork() returns the new pid to the parent process */

						if (childpid >= 0) /* fork succeeded */
						{
								if (childpid == 0) {
										/* the child continues as the actual slave */
										break;
								}
								else {
										DEBUG(LOG_NOTICE, "started child process %d", childpid);
										/* the parent keeps monitoring the slaves */ 
										cconf->pid = childpid;
								}
						}
						else /* fork returns -1 on failure */
						{
								perror("fork"); /* display error message */
								exit(0); 
						}
				} /* if pid==0 */

				if (cconf->next)
						/* continue to the next item on the list */
						cconf = cconf->next;
				else
						/* return to the start of the list */
						cconf = pconf;

				/* wait some time before the next iteration */
				threadsleep(0.25);
		} /* while monitoring */

#else
		if (numpeer!=1)
				PANIC("more than one slave not supported on windows");
		else
				/* continue with the first and only peerslave configuration */
				cconf = pconf;
#endif

		/*************************************************************************************
		 * from here onward the code deals with starting a single peer in slave mode
		 ************************************************************************************/

		/* get the values from the configuration structure */
		if (cconf->memavail)
		{
				pthread_mutex_lock(&mutexhost);
				host->memavail = atol(cconf->memavail);
				pthread_mutex_unlock(&mutexhost);
				pthread_mutex_lock(&mutexsmartmem);
				smartmem.enabled = 0;
				pthread_mutex_unlock(&mutexsmartmem);
		}

		if (cconf->cpuavail)
		{
				pthread_mutex_lock(&mutexhost);
				host->cpuavail = atol(cconf->cpuavail);
				pthread_mutex_unlock(&mutexhost);
		}

		if (cconf->timavail)
		{
				pthread_mutex_lock(&mutexhost);
				host->timavail = atol(cconf->timavail);
				pthread_mutex_unlock(&mutexhost);
		}

		if (cconf->hostname)
		{
				pthread_mutex_lock(&mutexhost);
				strncpy(host->name, cconf->hostname, STRLEN);
				pthread_mutex_unlock(&mutexhost);
		}

		if (cconf->group)
		{
				pthread_mutex_lock(&mutexhost);
				strncpy(host->group, cconf->group, STRLEN);
				pthread_mutex_unlock(&mutexhost);
		}

		if (cconf->allowuser)
		{
				str = strtok(cconf->allowuser, ",");
				while (str) {
						allowuser = (userlist_t *)malloc(sizeof(userlist_t));
						allowuser->name = malloc(STRLEN);
						strncpy(allowuser->name, str, STRLEN);
						allowuser->next = userlist;
						userlist = allowuser;
						str = strtok(NULL, ",");
				}
		}

		if (cconf->allowhost)
		{
				str = strtok(cconf->allowhost, ",");
				while (str) {
						allowhost = (hostlist_t *)malloc(sizeof(hostlist_t));
						allowhost->name = malloc(STRLEN);
						strncpy(allowhost->name, str, STRLEN);
						allowhost->next = hostlist;
						hostlist = allowhost;
						str = strtok(NULL, ",");
				}
		}

		if (cconf->allowgroup)
		{
				str = strtok(cconf->allowgroup, ",");
				while (str) {
						allowgroup = (grouplist_t *)malloc(sizeof(grouplist_t));
						allowgroup->name = malloc(STRLEN);
						strncpy(allowgroup->name, str, STRLEN);
						allowgroup->next = grouplist;
						grouplist = allowgroup;
						str = strtok(NULL, ",");
				}
		}

		if (cconf->matlab)
		{
				startcmd = malloc(STRLEN);
				strncpy(startcmd, cconf->matlab, STRLEN);
		}
		else
		{
				/* set the default command to start matlab */
				startcmd = malloc(STRLEN);
				strncpy(startcmd, STARTCMD, STRLEN);
		}

		if (cconf->smartmem)
		{
				pthread_mutex_lock(&mutexsmartmem);
				smartmem.enabled = atol(cconf->smartmem);
				pthread_mutex_unlock(&mutexsmartmem);
		}

		if (cconf->smartcpu)
		{
				pthread_mutex_lock(&mutexsmartcpu);
				smartcpu.enabled = atol(cconf->smartcpu);
				pthread_mutex_unlock(&mutexsmartcpu);
		}

		if (cconf->smartshare)
		{
				pthread_mutex_lock(&mutexsmartshare);
				smartshare.enabled = atol(cconf->smartshare);
				pthread_mutex_unlock(&mutexsmartshare);
		}

		if (cconf->timeout)
		{
				enginetimeout = atol(cconf->timeout);
		}

		if (udsserver_flag) {
				if ((rc = pthread_create(&udsserverThread, NULL, udsserver, (void *)NULL))>0) {
						PANIC("failed to start udsserver thread\n");
				}
				else {
						DEBUG(LOG_NOTICE, "started udsserver thread");
				}
		}

		if (tcpserver_flag) {
				if ((rc = pthread_create(&tcpserverThread, NULL, tcpserver, (void *)NULL))>0) {
						PANIC("failed to start tcpserver thread\n");
				}
				else {
						DEBUG(LOG_NOTICE, "started tcpserver thread");
				}
		}

		if ((rc = pthread_create(&announceThread, NULL, announce, (void *)NULL))>0) {
				PANIC("failed to start announce thread\n");
		}
		else {
				DEBUG(LOG_NOTICE, "started announce thread");
		}

		if ((rc = pthread_create(&discoverThread, NULL, discover, (void *)NULL))>0) {
				PANIC("failed to start discover thread\n");
		}
		else {
				DEBUG(LOG_NOTICE, "started discover thread");
		}

		if ((rc = pthread_create(&expireThread, NULL, expire, (void *)NULL))>0) {
				DEBUG(LOG_NOTICE, "failed to start expire thread");
				exit(1);
		}
		else {
				DEBUG(LOG_NOTICE, "started expire thread");
		}

		pthread_mutex_lock(&mutexhost);
		/* switch the peer to idle slave, note that this should be followed by an announce_once() */
		host->status = STATUS_IDLE;
		/* update the current job description */
		bzero(&(host->current), sizeof(current_t));
		pthread_mutex_unlock(&mutexhost);
		/* inform the other peers of the updated status */
		announce_once();

		/* engine aborted indicates that matlab crashed, in which case the peerslave should exit */
		while (!engineAborted) {

				/* switch the engine off after being idle for a certain time */
				if ((matlabRunning!=0) && (difftime(time(NULL), matlabFinished)>enginetimeout)) {
						if (engClose(en)!=0) {
								DEBUG(LOG_CRIT, "could not stop the MATLAB engine");
								matlabRunning = 0;
						}
						else {
								DEBUG(LOG_CRIT, "stopped idle MATLAB engine");
								matlabRunning = 0;
						}
				}

				/* switch from zombie to idle slave after waiting a certain time */
				if ((engineFailed!=0) && (difftime(time(NULL), engineFailed)>zombietimeout)) {
						DEBUG(LOG_NOTICE, "switching back to idle mode");
						pthread_mutex_lock(&mutexhost);
						/* make the slave available again, note that this should be followed by an announce_once() */
						host->status = STATUS_IDLE;
						/* update the current job description */
						bzero(&(host->current), sizeof(current_t));
						pthread_mutex_unlock(&mutexhost);
						/* inform the other peers of the updated status */
						announce_once();
						engineFailed  = 0;
						continue;
				}

				if (jobcount()>0) {
						/* there is a job to be executed */

						if (matlabRunning==0) {
								/* start the matlab engine */
								DEBUG(LOG_CRIT, "starting MATLAB engine");
								if ((en = engOpen(startcmd)) == NULL) {
										/* this is probably due to a licensing problem */
										/* do not start again during the timeout period */
										DEBUG(LOG_ERR, "could not start MATLAB engine, deleting job and switching to zombie");
										engineFailed  = time(NULL);
										matlabRunning = 0;

										pthread_mutex_lock(&mutexhost);
										/* switch the mode to busy slave, note that this should be followed by an announce_once() */
										host->status = STATUS_ZOMBIE;
										/* update the current job description */
										bzero(&(host->current), sizeof(current_t));
										pthread_mutex_unlock(&mutexhost);
										/* inform the other peers of the updated status */
										announce_once();
								}
								else {
										engineFailed  = 0;
										matlabRunning = 1;
								}
						}

						if (matlabRunning) {
								/* get the first job input arguments and options */
								pthread_mutex_lock(&mutexjoblist);
								job = joblist;

								pthread_mutex_lock(&mutexhost);
								/* switch the mode to busy slave, note that this should be followed by an announce_once() */
								host->status = STATUS_BUSY;
								/* update the current job description */
								bzero(&(host->current), sizeof(current_t));
								host->current.hostid= job->host->id;
								host->current.jobid = job->job->id;
								strncpy(host->current.name, job->host->name, STRLEN);
								strncpy(host->current.user, job->host->user, STRLEN);
								strncpy(host->current.group, job->host->group, STRLEN);
								host->current.timreq  = job->job->timreq;
								host->current.memreq  = job->job->memreq;
								host->current.cpureq  = job->job->cpureq;

								/* determine the maximum allowed job duration */
								timallow = 3*job->job->timreq;
								if (host->timavail < timallow)
										timallow = host->timavail;
								pthread_mutex_unlock(&mutexhost);

								/* inform the other peers of the updated status */
								announce_once();

								matlabStart = time(NULL);

								argin   = (mxArray *)mxDeserialize(job->arg, job->job->argsize);
								options = (mxArray *)mxDeserialize(job->opt, job->job->optsize);
								jobid   = job->job->id;
								peerid  = job->host->id;
								DEBUG(LOG_CRIT, "executing job %d from %s@%s (jobid=%u, memreq=%lu, timreq=%lu)", ++jobnum, job->host->user, job->host->name, job->job->id, job->job->memreq, job->job->timreq);
								pthread_mutex_unlock(&mutexjoblist);

								/* create a copy of the optin cell-array */
								n = mxGetM(options) * mxGetN(options);
								mxArray *previous = options;
								options = mxCreateCellMatrix(1, n+4);
								for (i=0; i<n; i++)
										mxSetCell(options, i, mxGetCell(previous, i));
								/* add the masterid and timallow options, these are used by peerexec for the watchdog */
								mxSetCell(options, n+0, mxCreateString("masterid\0"));
								mxSetCell(options, n+1, mxCreateDoubleScalar(peerid));
								mxSetCell(options, n+2, mxCreateString("timallow\0"));
								mxSetCell(options, n+3, mxCreateDoubleScalar(timallow));

								jobFailed = 0;

								/* copy the input arguments and options over to the engine */
								if (!jobFailed && (engPutVariable(en, "argin", argin) != 0)) {
										DEBUG(LOG_ERR, "error copying argin variable to engine");
										jobFailed = 1;
								}

								if (!jobFailed && (engPutVariable(en, "options", options) != 0)) {
										DEBUG(LOG_ERR, "error copying options variable to engine");
										jobFailed = 2;
								}

								mxDestroyArray(argin);
								mxDestroyArray(options);

								/* execute the job */
								if (!jobFailed && (engEvalString(en, "[argout, options] = peerexec(argin, options);") != 0)) {
										DEBUG(LOG_ERR, "error evaluating string in engine");
										jobFailed = 3;
										engineAborted = 1;
								}

								/* get the output arguments and options */
								if (!jobFailed && (argout = engGetVariable(en, "argout")) == NULL) {
										DEBUG(LOG_ERR, "error getting argout");
										jobFailed = 4;
										engineAborted = 1;
								}

								if (!jobFailed && (options = engGetVariable(en, "options")) == NULL) {
										DEBUG(LOG_ERR, "error getting options");
										jobFailed = 5;
										engineAborted = 1;
								}

						} /* if matlabRunning */

						if (engineFailed || jobFailed) {
								/* there was a problem executing the job */
								pthread_mutex_lock(&mutexjoblist);
								job     = joblist;
								jobid   = job->job->id;
								peerid  = job->host->id;
								if (engineFailed) {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s (engine)", jobnum, job->host->user, job->host->name);
								}
								else if (jobFailed==1) {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s (argin)", jobnum, job->host->user, job->host->name);
								}
								else if (jobFailed==2) {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s (optin)", jobnum, job->host->user, job->host->name);
								}
								else if (jobFailed==3) {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s (eval)", jobnum, job->host->user, job->host->name);
								}
								else if (jobFailed==4) {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s (argout)", jobnum, job->host->user, job->host->name);
								}
								else if (jobFailed==5) {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s (optout)", jobnum, job->host->user, job->host->name);
								}
								else {
										DEBUG(LOG_CRIT, "failed to execute job %d from %s@%s", jobnum, job->host->user, job->host->name);
								}
								pthread_mutex_unlock(&mutexjoblist);
						}

						if (engineFailed) {
								/* create the ouptut argument containing an error */
								argout = mxCreateCellMatrix(1,1);
								/* specify the error in the options */
								options = mxCreateCellMatrix(1,2);
								mxSetCell(options, 0, mxCreateString("lasterr\0"));
								mxSetCell(options, 1, mxCreateString("could not start the matlab engine\0"));
						}
						else if (jobFailed) {
								/* create the ouptut argument containing an error */
								argout = mxCreateCellMatrix(1,1);
								/* specify the error in the options */
								options = mxCreateCellMatrix(1,2);
								mxSetCell(options, 0, mxCreateString("lasterr\0"));
								if (jobFailed==1)
										mxSetCell(options, 1, mxCreateString("failed to execute the job (argin)\0"));
								else if (jobFailed==2)
										mxSetCell(options, 1, mxCreateString("failed to execute the job (optin)\0"));
								else if (jobFailed==3)
										mxSetCell(options, 1, mxCreateString("failed to execute the job (eval)\0"));
								else if (jobFailed==4)
										mxSetCell(options, 1, mxCreateString("failed to execute the job (argout)\0"));
								else if (jobFailed==5)
										mxSetCell(options, 1, mxCreateString("failed to execute the job (optout)\0"));
								else
										mxSetCell(options, 1, mxCreateString("failed to execute the job\0"));
						}

						/*****************************************************************************
						 * send the results back to the master
						 * the following code is largely shared with the put-option in the peer mex file
						 *****************************************************************************/
						pthread_mutex_lock(&mutexpeerlist);
						found = 0;

						peer = peerlist;
						while(peer) {
								if (peer->host->id==peerid) {
										found = 1;
										break;
								}
								peer = peer->next ;
						}

						if (!found) {
								pthread_mutex_unlock(&mutexpeerlist);
								DEBUG(LOG_ERR, "failed to locate specified peer");
								goto cleanup;
						}

						pthread_mutex_lock(&mutexhost);
						int hasuds = (strlen(peer->host->socket)>0 && strcmp(peer->host->name, host->name)==0);
						int hastcp = (peer->host->port>0);
						pthread_mutex_unlock(&mutexhost);

						/* these have to be initialized to NULL to ensure that the cleanup works */
						server = 0;
						def = NULL;
						arg = NULL;
						opt = NULL;

						if (hasuds) {
								/* open the UDS socket */
								if ((server = open_uds_connection(peer->host->socket)) < 0) {
										pthread_mutex_unlock(&mutexpeerlist);
										DEBUG(LOG_ERR, "failed to create socket");
										goto cleanup;
								}
						}
						else if (hastcp) {
								/* open the TCP socket */
								if ((server = open_tcp_connection(peer->ipaddr, peer->host->port)) < 0) {
										pthread_mutex_unlock(&mutexpeerlist);
										DEBUG(LOG_ERR, "failed to create socket");
										goto cleanup;
								}
						}
						else {
								pthread_mutex_unlock(&mutexpeerlist);
								DEBUG(LOG_ERR, "failed to create socket");
								goto cleanup;
						}

						/* the connection was opened without error */
						pthread_mutex_unlock(&mutexpeerlist);

						if ((n = bufread(server, &handshake, sizeof(int))) != sizeof(int)) {
								DEBUG(LOG_ERR, "could not read handshake");
								goto cleanup;
						}
						else if (!handshake) {
								DEBUG(LOG_ERR, "failed to negociate connection");
								goto cleanup;
						}

						/* the message that will be written consists of
						   message->host
						   message->job
						   message->arg
						   message->opt
						 */

						if ((arg = (mxArray *) mxSerialize(argout))==NULL) {
								DEBUG(LOG_ERR, "could not serialize job arguments");
								goto cleanup;
						}

						if ((opt = (mxArray *) mxSerialize(options))==NULL) {
								DEBUG(LOG_ERR, "could not serialize job options");
								goto cleanup;
						}

						if ((def = (jobdef_t *)malloc(sizeof(jobdef_t)))==NULL) {
								DEBUG(LOG_ERR, "could not allocate memory");
								goto cleanup;
						}

						def->version  = VERSION;
						def->id       = jobid;
						def->memreq   = 0;
						def->cpureq   = 0;
						def->timreq   = 0;
						def->argsize  = mxGetNumberOfElements(arg);
						def->optsize  = mxGetNumberOfElements(opt);

						/* write the message  (hostdef, jobdef, arg, opt) with handshakes in between */
						/* the slave may close the connection between the message segments in case the job is refused */
						success = 1;

						pthread_mutex_lock(&mutexhost);
						if (success) 
								success = (bufwrite(server, host, sizeof(hostdef_t)) == sizeof(hostdef_t));
						pthread_mutex_unlock(&mutexhost);

						if ((n = bufread(server, &handshake, sizeof(int))) != sizeof(int)) {
								DEBUG(LOG_ERR, "could not read handshake");
								goto cleanup;
						}
						else if (!handshake) {
								DEBUG(LOG_ERR, "failed to write hostdef");
								goto cleanup;
						}

						if (success)
								success = (bufwrite(server, def, sizeof(jobdef_t)) == sizeof(jobdef_t));

						if ((n = bufread(server, &handshake, sizeof(int))) != sizeof(int)) {
								DEBUG(LOG_ERR, "could not read handshake");
								goto cleanup;
						}
						else if (!handshake) {
								DEBUG(LOG_ERR, "failed to write jobdef");
								goto cleanup;
						}

						if (success) 
								success = (bufwrite(server, (void *)mxGetData(arg), def->argsize) == def->argsize);

						if ((n = bufread(server, &handshake, sizeof(int))) != sizeof(int)) {
								DEBUG(LOG_ERR, "could not read handshake");
								goto cleanup;
						}
						else if (!handshake) {
								DEBUG(LOG_ERR, "failed to write arg");
								goto cleanup;
						}

						if (success) 
								success = (bufwrite(server, (void *)mxGetData(opt), def->optsize) == def->optsize);

						if ((n = bufread(server, &handshake, sizeof(int))) != sizeof(int)) {
								DEBUG(LOG_ERR, "could not read handshake");
								goto cleanup;
						}
						else if (!handshake) {
								DEBUG(LOG_ERR, "failed to write opt");
								goto cleanup;
						}

cleanup:
						if (server>0) {
								close_connection(server);
								server = 0;
						}

						if (arg) {
								mxDestroyArray(arg);
								arg = NULL;
						}

						if (opt) {
								mxDestroyArray(opt);
								opt = NULL;
						}

						FREE(def);

						/*****************************************************************************/

						/* clear the joblist */
						clear_joblist();

						if (!engineFailed) {
								pthread_mutex_lock(&mutexhost);
								/* make the slave available again, note that this should be followed by an announce_once() */
								host->status = STATUS_IDLE;
								/* update the current job description */
								bzero(&(host->current), sizeof(current_t));
								pthread_mutex_unlock(&mutexhost);
								/* inform the other peers of the updated status */
								announce_once();

								matlabFinished = time(NULL);
								DEBUG(LOG_CRIT, "executing job %d took %d seconds", jobnum, matlabFinished - matlabStart);
						}
				}
				else {
						threadsleep(SLEEPTIME);
				} /* if jobcount */

		} /* while not engineAborted */

		return engineAborted;
} /* main */

