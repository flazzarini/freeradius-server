/*
 * otp_cardops.c
 * $Id$
 *
 * Passcode verification functions for otp.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * Copyright 2002-2005  Google, Inc.
 * Copyright 2005 Frank Cusack
 *
 */

static const char rcsid[] = "$Id$";

#include <stdlib.h>
#include <string.h>

#include "otp.h"
#include "otp_cardops.h"

/* Global data */
cardops_t otp_cardops[OTP_MAX_VENDORS];	/* cardops objects */
int otp_num_cardops = 0;		/* number of cardops modules loaded */

/*
 * Test for passcode validity.
 *
 * If challenge is supplied, it is used to generate the card response
 * against which the passcode will be compared.  If challenge is not
 * supplied, or if the comparison fails, synchronous responses are
 * generated and tested.  NOTE: for async authentications, sync mode
 * responses are still considered valid!  (Assuming module configuration
 * allows sync mode.)
 *
 * If passcode is supplied, a simple string comparison is done, else if
 * cmp is supplied, it is called to test for validity.  The cmp function
 * is required for RADIUS, where we might have a CHAP response instead
 * of the plaintext of the passcode from the user.
 *
 * If challenge is supplied, then resync is used to determine if the card
 * should be resynced or if this is a one-off response.  (If challenge is
 * not supplied, this is sync mode response and the card is always resynced.)
 *
 * Returns one of the OTP_RC_* return codes.
 */
int
otp_pw_valid(const char *username, char *challenge, const char *passcode,
	     int resync, const otp_option_t *opt,
	     cmpfunc_t cmpfunc, void *data,
	     const char *log_prefix)
{
    int		rc, fc, nmatch, i;

    		/* expected response */
    char	e_response[OTP_MAX_RESPONSE_LEN + OTP_MAX_PIN_LEN + 1];
    int		pin_adjust = 0;	/* pin offset in e_response */
    unsigned	auth_pos = 0;	/* window position of this authentication */
    time_t	last_auth_time;	/* time of last authentication */

    otp_user_info_t	user_info = { .cardops = NULL };

    /* sanity */
    if (!challenge) {
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
    }
    if (!passcode && !cmpfunc) {
	otp_log(OTP_LOG_CRIT, "%s: Can't test passcode validity!", log_prefix);
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
    }

    /* Look up user info. */
    rc = otp_get_user_info(opt->pwdfile, username, &user_info);
    if (rc == -1) {
	otp_log(OTP_LOG_INFO, "%s: user [%s] not found in %s",
		log_prefix, username, opt->pwdfile);
	rc = OTP_RC_USER_UNKNOWN;
	goto auth_done_service_err;
    } else if (rc == -2) {
#if 0
	/* otp_get_user_info() logs a more useful message, this is noisy. */
	otp_log(OTP_LOG_ERR, "%s: error reading user [%s] info",
		log_prefix, username);
#endif
	rc = OTP_RC_AUTHINFO_UNAVAIL;
	goto auth_done_service_err;
    }
    user_info.username = username;

    /* Find the correct cardops module. */
    for (i = 0; otp_cardops[i].prefix; i++) {
	if (!strncasecmp(otp_cardops[i].prefix, user_info.card,
			 otp_cardops[i].prefix_len)) {
	    user_info.cardops = &otp_cardops[i];
	    break;
	}
    }
    if (!user_info.cardops) {
	otp_log(OTP_LOG_ERR,
		"%s: invalid card type '%s' for [%s]",
		log_prefix, user_info.card, username);
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
    }

    /* Convert name to a feature mask once, for fast operations later. */
    if (user_info.cardops->name2fm(user_info.card, &user_info.featuremask)) {
	otp_log(OTP_LOG_ERR,
		"%s: invalid card type '%s' for [%s]",
		log_prefix, user_info.card, username);
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
    }

    /* Convert keystring to a keyblock. */
    if (user_info.cardops->keystring2keyblock(user_info.keystring,
					      user_info.keyblock)) {
	otp_log(OTP_LOG_ERR,
		"%s: invalid key '%s' for [%s]",
		log_prefix, user_info.keystring, username);
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
    }

    /* Adjust e_response for PIN prepend. */
    if (opt->prepend_pin) {
	(void) strcpy(e_response, user_info.pin);
	pin_adjust = strlen(e_response);
    }

    /* Get the time of the last authentication. */
    if (otp_get_last_auth(opt->syncdir, username, &last_auth_time) != 0) {
	otp_log(OTP_LOG_ERR,
		"%s: unable to get last auth time for [%s]",
		log_prefix, username);
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
	/* NB: last_auth_time, failcount not updated. */
    }

    /* Get failure count for later evaluation. */
    fc = otp_check_failcount(username, opt);
    if (fc == OTP_FC_FAIL_ERR) {
	rc = OTP_RC_SERVICE_ERR;
	goto auth_done_service_err;
	/* NB: last_auth_time, failcount not updated. */
    }

async_response:
    /*
     * Test async response.
     */
    if (*challenge && (user_info.featuremask & OTP_CF_AM) && opt->allow_async){
	/* Perform any site-specific transforms of the challenge. */
	if (otp_challenge_transform(username, challenge) != 0) {
	    otp_log(OTP_LOG_ERR, "%s: challenge transform failed for [%s]",
		    log_prefix, username);
	    rc = OTP_RC_SERVICE_ERR;
	    goto auth_done_service_err;
	    /* NB: last_auth_time, failcount not updated. */
	}

	/* Calculate the async response. */
	if (user_info.cardops->response(&user_info, challenge,
					&e_response[pin_adjust]) != 0) {
	    otp_log(OTP_LOG_ERR,
		    "%s: unable to calculate async response for [%s], "
		    "to challenge %s", log_prefix, username, challenge);
	    rc = OTP_RC_SERVICE_ERR;
	    goto auth_done_service_err;
	    /* NB: last_auth_time, failcount not updated. */
	}
	/* NOTE: We do not display the PIN. */
#if defined(FREERADIUS)
	DEBUG("rlm_otp_token: auth: [%s], async challenge %s, "
	      "expecting response %s", username, challenge,
	      &e_response[pin_adjust]);
#elif defined(PAM)
	if (opt->debug)
	    otp_log(OTP_LOG_DEBUG, "%s: [%s], async challenge %s, "
				   "expecting response %s",
		    log_prefix, username, challenge, &e_response[pin_adjust]);
#endif

	/* Add PIN if needed. */
	if (!opt->prepend_pin)
	    (void) strcat(e_response, user_info.pin);

	/* Test user-supplied passcode. */
	if (passcode)
	    nmatch = strcmp(passcode, e_response);
	else
	    nmatch = cmpfunc(data, e_response);
	if (!nmatch) {
	    if (!opt->allow_async) {
		otp_log(OTP_LOG_AUTH,
			"%s: bad async auth for [%s]: "
			"valid but disallowed by config",
			log_prefix, username);
		rc = OTP_RC_AUTH_ERR;
		goto auth_done;
	    }
	    if (fc == OTP_FC_FAIL_HARD) {
		otp_log(OTP_LOG_AUTH,
			"%s: bad async auth for [%s]: valid but in hardfail",
			log_prefix, username);
		rc = OTP_RC_MAXTRIES;
		goto auth_done;
	    }
	    if (fc == OTP_FC_FAIL_SOFT) {
		otp_log(OTP_LOG_AUTH,
			"%s: bad async auth for [%s]: valid but in softfail",
			log_prefix, username);
		rc = OTP_RC_MAXTRIES;
		goto auth_done;
	    }
#ifdef FREERADIUS
	    if (time(NULL) - last_auth_time < opt->chal_delay) {
		otp_log(OTP_LOG_AUTH,
			"%s: bad async auth for [%s]: valid but too soon",
			log_prefix, username);
		rc = OTP_RC_MAXTRIES;
		goto auth_done;

	    }
#endif

	    /* Authenticated in async mode. */
	    rc = OTP_RC_OK;
	    /* special log message for sync users */
	    if (user_info.featuremask & OTP_CF_SM)
		otp_log(OTP_LOG_INFO, "%s: [%s] authenticated in async mode",
			log_prefix, username);
	    goto auth_done;
	} /* if (user authenticated async) */
    } /* if (async mode possible) */

sync_response:
    /*
     * Calculate and test sync responses in the window.
     * Note that we always accept a sync response, even
     * if a challenge or resync was explicitly requested.
     */
    if ((user_info.featuremask & OTP_CF_SM) && opt->allow_sync) {
	int start = 0, end = opt->ewindow_size, last_auth_pos = 0;

	/* Increase window for ewindow2. */
	if (opt->ewindow2_size && fc == OTP_FC_FAIL_SOFT) {
	    last_auth_pos = otp_get_last_auth_pos(opt->syncdir, username);
	    if (last_auth_pos < 0) {
		otp_log(OTP_LOG_ERR,
			"%s: unable to get last auth window position for [%s]",
			log_prefix, username);
		rc = OTP_RC_SERVICE_ERR;
		goto auth_done_service_err;
		/* NB: last_auth_time, failcount not updated. */
	    }
	    end = opt->ewindow2_size;
	}

        for (i = start; i <= end; ++i) {
	    /* Get sync challenge and key. */
	    if (user_info.cardops->challenge(opt->syncdir, &user_info,
					     i, 0, challenge) != 0) {
		otp_log(OTP_LOG_ERR,
			"%s: unable to get sync challenge e:%d t:%d for [%s]",
			log_prefix, i, 0, username);
		rc = OTP_RC_SERVICE_ERR;
		goto auth_done_service_err;
		/* NB: last_auth_time, failcount not updated. */
	    }

	    /* Calculate sync response. */
	    if (user_info.cardops->response(&user_info, challenge,
					    &e_response[pin_adjust]) != 0) {
		otp_log(OTP_LOG_ERR,
			"%s: unable to calculate sync response "
			"e:%d t:%d for [%s], to challenge %s",
			log_prefix, i, 0, username, challenge);
		rc = OTP_RC_SERVICE_ERR;
		goto auth_done_service_err;
		/* NB: last_auth_time, failcount not updated. */
	    }
	    /* NOTE: We do not display the PIN. */
#if defined(FREERADIUS)
	    DEBUG("rlm_otp_token: auth: [%s], sync challenge %d %s, "
		  "expecting response %s", username, i, challenge,
		  &e_response[pin_adjust]);
#elif defined(PAM)
	    if (opt->debug)
		otp_log(OTP_LOG_DEBUG, "%s: [%s], sync challenge %d %s, "
				       "expecting response %s",
			log_prefix, username, i,
			challenge, &e_response[pin_adjust]);
#endif

	    /* Add PIN if needed. */
	    if (!opt->prepend_pin)
		(void) strcat(e_response, user_info.pin);

	    /* Test user-supplied passcode. */
	    if (passcode)
		nmatch = strcmp(passcode, e_response);
	    else
		nmatch = cmpfunc(data, e_response);
	    if (!nmatch) {
		if (fc == OTP_FC_FAIL_HARD) {
		    otp_log(OTP_LOG_AUTH, "%s: bad sync auth for [%s]: "
					  "valid but in hardfail",
			    log_prefix, username);
		    rc = OTP_RC_MAXTRIES;
		    goto auth_done;
		}

		/*
		 * ewindow2_size logic
		 */
		if (fc == OTP_FC_FAIL_SOFT) {
		    if (!opt->ewindow2_size) {
			/* ewindow2 mode not configured */
			otp_log(OTP_LOG_AUTH, "%s: bad sync auth for [%s]: "
					      "valid but in softfail",
				log_prefix, username);
			rc = OTP_RC_MAXTRIES;
			goto auth_done;
		    }

		    /*
		     * User must enter two consecutive correct sync passcodes
		     * for ewindow2 softfail override.
		     *
		     * last_auth_pos == 0 could mean that the last entry was
		     * correct and at the zeroeth sync position, or that the
		     * last entry was correct and async, or that the last
		     * entry was incorrect.  Since we can't differentiate,
		     * we can't use a 0 last_auth_pos as the first passcode
		     * in the ewindow2 sequence.  This means that users who
		     * start an ewindow2 softfail override at the very left
		     * edge of the window must enter 3 passcodes (0,1,2)
		     * instead of 2.
		     * TODO: update get_last_auth_pos to return pos+1.
		     */
		    if (last_auth_pos && (i == last_auth_pos + 1) &&
			/* ... within ewindow2_delay seconds. */
			(time(NULL) - last_auth_time < opt->ewindow2_delay)) {
			/* This is the 2nd of two consecutive responses. */
			otp_log(OTP_LOG_AUTH,
				"%s: ewindow2 softfail override for [%s] at "
				"window position %d", log_prefix, username, i);
		    } else {
			/* correct, but not consecutive or not soon enough */
#if defined(FREERADIUS)
			DEBUG("rlm_otp_token: auth: [%s] ewindow2 candidate "
			      "at position %i", username, i);
#elif defined(PAM)
			if (opt->debug)
			    otp_log(OTP_LOG_DEBUG,
				    "%s: auth: [%s] ewindow2 candidate "
				    "at position %i", log_prefix, username, i);
#endif
			auth_pos = i;
			rc = OTP_RC_AUTH_ERR;
			goto auth_done;
		    }
		}

		/* Authenticated in sync mode. */
		rc = OTP_RC_OK;
		resync = 1;
		goto auth_done;

	    } /* if (passcode is valid) */
	} /* for (each slot in the window) */
    } /* if (sync mode possible) */

    /* Both async and sync mode failed. */
    rc = OTP_RC_AUTH_ERR;

auth_done:
    if (rc == OTP_RC_OK) {
	if (resync) {
	    /*
	     * Resync the card.
	     *
	     * We "fail-out" if we can't do this, because for sync mode the
	     * response can be reused until sync data is updated, an obvious
	     * replay attack.
	     *
	     * For async mode with RADIUS, if we can't update the last auth
	     * time (a side effect of otp_set_sync_data()), we will be open
	     * to a less obvious replay attack over the lifetime of the State
	     * attribute (opt->chal_delay): if someone that can see RADIUS
	     * traffic captures an Access-Request containing a State
	     * attribute, and can cause the NAS to cycle the request id
	     * within opt->chal_delay secs, then they can login to the NAS
	     * and insert the captured State attribute into the new
	     * Access-Request, and we'll give an Access-Accept.
	     */
	    if (user_info.cardops->challenge(opt->syncdir, &user_info,
					     1, 0, challenge) != 0) {
		otp_log(OTP_LOG_ERR, "%s: unable to get sync challenge "
			"e:%d t:%d for [%s] (for resync)",
			log_prefix, 1, 0, username);
		rc = OTP_RC_SERVICE_ERR;
	    } else if (otp_set_sync_data(opt->syncdir, username, challenge,
					 user_info.keyblock) != 0) {
		otp_log(OTP_LOG_ERR,
			"%s: unable to set sync data for [%s] (for resync)",
			log_prefix, username);
		rc = OTP_RC_SERVICE_ERR;
	    }
	} else {
	    /* Just update failcount, last_auth_time, auth_pos. */
	    if (otp_reset_failcount(opt->syncdir, username) != 0) {
		otp_log(OTP_LOG_ERR,
			"%s: unable to reset failcount for [%s]",
			log_prefix, username);
		rc = OTP_RC_SERVICE_ERR;
	    }
	}
    } else {
	if (otp_incr_failcount(opt->syncdir, username) != 0) {
	    otp_log(OTP_LOG_ERR,
		    "%s: unable to increment failure count for user [%s]",
		    log_prefix, username);
	    rc = OTP_RC_SERVICE_ERR;
	}
	if (otp_set_last_auth_pos(opt->syncdir, username, auth_pos)) {
	    otp_log(OTP_LOG_ERR,
		    "%s: unable to set auth window position for user [%s]",
		    log_prefix, username);
	    rc = OTP_RC_SERVICE_ERR;
	}
	/*
	 * TODO: consolidate reset_failcount, incr_failcount, s_l_a_p
	 * into set_sync_data with longterm lock.
	 */
    }

auth_done_service_err:	/* exit here for system errors */
    return rc;
}
