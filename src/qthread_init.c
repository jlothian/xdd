/* Copyright (C) 1992-2010 I/O Performance, Inc. and the
 * United States Departments of Energy (DoE) and Defense (DoD)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named 'Copying'; if not, write to
 * the Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139.
 */
/* Principal Author:
 *      Tom Ruwart (tmruwart@ioperformance.com)
 * Contributing Authors:
 *       Steve Hodson, DoE/ORNL
 *       Steve Poole, DoE/ORNL
 *       Bradly Settlemyer, DoE/ORNL
 *       Russell Cattelan, Digital Elves
 *       Alex Elder
 * Funding and resources provided by:
 * Oak Ridge National Labs, Department of Energy and Department of Defense
 *  Extreme Scale Systems Center ( ESSC ) http://www.csm.ornl.gov/essc/
 *  and the wonderful people at I/O Performance, Inc.
 */
#include "xdd.h"

/*----------------------------------------------------------------------------*/
/* xdd_qthread_init() - Initialization routine for a QThread
 * This routine is passed a pointer to the PTDS for this QThread.
 * Return values: 0 is good, -1 is bad
 */
int32_t
xdd_qthread_init(ptds_t *qp) {
	int32_t  	status;
	ptds_t		*p;			// Pointer to this qthread's target PTDS
	char		tmpname[XDD_BARRIER_NAME_LENGTH];	// Used to create unique names for the barriers


	// Get the target Thread PTDS address as well
	p = qp->target_ptds;

	// Open the target device/file
	status = xdd_target_open(qp);
	if (status < 0) {
		fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: ERROR: Failed to open Target named '%s'\n",
			xgp->progname,
			qp->my_target_number,
			qp->my_qthread_number,
			qp->target_full_pathname);
		return(-1);
	}

	// Get a RW buffer
	qp->rwbuf = xdd_init_io_buffers(qp);
	if (qp->rwbuf == 0) {
		fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: ERROR: Failed to allocate I/O buffer.\n",
			xgp->progname,
			qp->my_target_number,
			qp->my_qthread_number);
		return(-1);
	}

	// Set proper data pattern in RW buffer
	xdd_datapattern_buffer_init(qp);

	// Init the QThread-TargetPass WAIT Barrier for this QThread
	sprintf(tmpname,"T%04d:Q%04d>qthread_targetpass_wait_barrier",qp->my_target_number,qp->my_qthread_number);
	status = xdd_init_barrier(&qp->qthread_targetpass_wait_barrier, 2, tmpname);
	if (status) {
		fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: ERROR: Cannot initialize QThread TargetPass WAIT barrier.\n",
			xgp->progname, 
			qp->my_target_number,
			qp->my_qthread_number);
		fflush(xgp->errout);
		return(-1);
	}

	
	// Initialize the inter-qthread semaphore - this is a "non-shared" semaphore 
	// By default this is initialized but can be overriden by the -nopocsem option
	if (!(qp->target_options & TO_NO_POC_SEMAPHORE)) {
		status = sem_init(&qp->qthread_task_complete, 0, 0);
		if (status) {
			fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: ERROR: Cannot initialize qthread_task_complete semaphore.\n",
				xgp->progname, 
				qp->my_target_number,
				qp->my_qthread_number);
			fflush(xgp->errout);
			return(-1);
		}
	}
	status = sem_init(&qp->this_qthread_available, 0, 1);
	if (status) {
		fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: ERROR: Cannot initialize this_qthread_available semaphore.\n",
			xgp->progname, 
			qp->my_target_number,
			qp->my_qthread_number);
		fflush(xgp->errout);
		return(-1);
	}
	qp->qthread_to_wait_for = NULL;

	// Set up for an End-to-End operation (if requested)
	if (qp->target_options & TO_ENDTOEND) {
		qp->e2e_sr_time = 0;
		if (qp->target_options & TO_E2E_DESTINATION) { // This is the Destination side of an End-to-End
			status = xdd_e2e_dest_init(qp);
		} else if (qp->target_options & TO_E2E_SOURCE) { // This is the Source side of an End-to-End
			status = xdd_e2e_src_init(qp);
		} else { // Not sure which side of the E2E this target is supposed to be....
			fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: Cannot determine which side of the E2E operation this target is supposed to be.\n",
				xgp->progname,
				qp->my_target_number,
				qp->my_qthread_number);
			fprintf(xgp->errout,"%s: xdd_qthread_init: Check to be sure that the '-e2e issource' or '-e2e isdestination' was specified for this target.\n",
				xgp->progname);
				fflush(xgp->errout);
			return(-1);
		}
		if (status == -1) {
			fprintf(xgp->errout,"%s: xdd_qthread_init: Target %d QThread %d: E2E %s initialization failed.\n",
				xgp->progname,
				qp->my_target_number,
				qp->my_qthread_number,
				(qp->target_options & TO_E2E_DESTINATION) ? "DESTINATION":"SOURCE");
		return(-1);
		}
	} // End of end-to-end setup
	qp->my_current_state |= CURRENT_STATE_THIS_QTHREAD_IS_AVAILABLE;

	// All went well...
	return(0);

} // End of xdd_qthread_init()
 