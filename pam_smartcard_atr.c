#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <PCSC/winscard.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include <security/pam_appl.h>

// Configuration file path
#define CONFIG_FILE "/etc/smartcard_login.conf"

// Function to get card ATR
static char* get_card_atr() {
    SCARDCONTEXT hContext;
    DWORD dwReaders = SCARD_AUTOALLOCATE;
    LPSTR mszReaders = NULL;
    char* atr = NULL;
    
    if (SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &hContext) != SCARD_S_SUCCESS) {
        return NULL;
    }
    
    if (SCardListReaders(hContext, NULL, (LPSTR)&mszReaders, &dwReaders) == SCARD_S_SUCCESS) {
        char* reader = mszReaders;
        while (*reader) {
            SCARDHANDLE hCard;
            DWORD dwActiveProtocol;
            
            if (SCardConnect(hContext, reader, SCARD_SHARE_SHARED, 
                           SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &hCard, &dwActiveProtocol) == SCARD_S_SUCCESS) {
                
                BYTE pbAtr[MAX_ATR_SIZE];
                DWORD dwAtrLen = sizeof(pbAtr);
                DWORD dwReaderLen = 0, dwState = 0, dwProtocol = 0;
                
                if (SCardStatus(hCard, NULL, &dwReaderLen, &dwState, &dwProtocol, pbAtr, &dwAtrLen) == SCARD_S_SUCCESS) {
                    atr = malloc(dwAtrLen * 2 + 1);
                    if (atr) {
                        for (DWORD i = 0; i < dwAtrLen; i++) {
                            sprintf(atr + i * 2, "%02X", pbAtr[i]);
                        }
                        atr[dwAtrLen * 2] = '\0';
                    }
                }
                
                SCardDisconnect(hCard, SCARD_LEAVE_CARD);
                break;
            }
            reader += strlen(reader) + 1;
        }
        SCardFreeMemory(hContext, mszReaders);
    }
    
    SCardReleaseContext(hContext);
    return atr;
}

// Function to check if ATR matches user
static int check_atr_user(const char* atr, const char* username) {
    FILE* config = fopen(CONFIG_FILE, "r");
    if (!config) {
        return 0;
    }
    
    char line[512];
    while (fgets(line, sizeof(line), config)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char* colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char* user = colon + 1;
            
            // Remove newline from username
            char* newline = strchr(user, '\n');
            if (newline) *newline = '\0';
            
            if (strcmp(line, atr) == 0 && strcmp(user, username) == 0) {
                fclose(config);
                return 1;
            }
        }
    }
    
    fclose(config);
    return 0;
}

// PAM authentication function
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    const char* username;
    char* card_atr;
    int retval;
    
    // Get username
    retval = pam_get_user(pamh, &username, NULL);
    if (retval != PAM_SUCCESS) {
        return PAM_AUTH_ERR;
    }
    
    // Try to get card ATR, if not present use delayed authentication
    card_atr = get_card_atr();
    if (!card_atr) {
        // Display waiting message
        struct pam_conv *conv;
        retval = pam_get_item(pamh, PAM_CONV, (const void**)&conv);
        if (retval == PAM_SUCCESS && conv && conv->conv) {
            char wait_msg[256];
            snprintf(wait_msg, sizeof(wait_msg), "Insert smart card...");
            
            struct pam_message msg;
            const struct pam_message *msgs[1];
            struct pam_response *resp = NULL;
            
            msg.msg_style = PAM_TEXT_INFO;
            msg.msg = wait_msg;
            msgs[0] = &msg;
            
            conv->conv(1, msgs, &resp, conv->appdata_ptr);
            if (resp) {
                if (resp[0].resp) free(resp[0].resp);
                free(resp);
            }
        }

        // Wait and check for card insertion every 0.5 seconds for up to 5 seconds
        for (int i = 0; i < 10 && !card_atr; i++) {
            usleep(500000); // Wait 0.5 seconds
            card_atr = get_card_atr();
            
            if (card_atr && conv && conv->conv) {
                // Display card detected message
                char detected_msg[256];
                snprintf(detected_msg, sizeof(detected_msg), "Smart card detected");
                
                struct pam_message msg;
                const struct pam_message *msgs[1];
                struct pam_response *resp = NULL;
                
                msg.msg_style = PAM_TEXT_INFO;
                msg.msg = detected_msg;
                msgs[0] = &msg;
                
                conv->conv(1, msgs, &resp, conv->appdata_ptr);
                if (resp) {
                    if (resp[0].resp) free(resp[0].resp);
                    free(resp);
                }
                break;
            }
        }
        
        if (!card_atr) {
            // Display timeout message
            if (conv && conv->conv) {
                char timeout_msg[256];
                snprintf(timeout_msg, sizeof(timeout_msg), "Card not found");
                
                struct pam_message msg;
                const struct pam_message *msgs[1];
                struct pam_response *resp = NULL;
                
                msg.msg_style = PAM_TEXT_INFO;
                msg.msg = timeout_msg;
                msgs[0] = &msg;
                
                conv->conv(1, msgs, &resp, conv->appdata_ptr);
                if (resp) {
                    if (resp[0].resp) free(resp[0].resp);
                    free(resp);
                }
            }
            
            syslog(LOG_AUTH | LOG_INFO, "pam_smartcard_atr: No smart card inserted within timeout for user %s, falling back to password", username);
            return PAM_AUTH_ERR;
        }
    }
    
    // Check if ATR matches username
    if (check_atr_user(card_atr, username)) {
        // Display success message to user
        char success_msg[256];
        snprintf(success_msg, sizeof(success_msg), "Smart card recognized");
        
        struct pam_message msg;
        const struct pam_message *msgs[1];
        struct pam_response *resp = NULL;
        struct pam_conv *conv;
        
        msg.msg_style = PAM_TEXT_INFO;
        msg.msg = success_msg;
        msgs[0] = &msg;
        
        // Get conversation function and display message
        retval = pam_get_item(pamh, PAM_CONV, (const void**)&conv);
        if (retval == PAM_SUCCESS && conv && conv->conv) {
            conv->conv(1, msgs, &resp, conv->appdata_ptr);
            if (resp) {
                if (resp[0].resp) {
                    free(resp[0].resp);
                }
                free(resp);
            }
        }
        
        syslog(LOG_AUTH | LOG_INFO, "pam_smartcard_atr: Smart card authentication successful for user %s", username);
        free(card_atr);
        return PAM_SUCCESS;
    } else {
        // Display failure message to user
        char failure_msg[256];
        snprintf(failure_msg, sizeof(failure_msg), "Smart card not recognized");
        
        struct pam_message msg;
        const struct pam_message *msgs[1];
        struct pam_response *resp = NULL;
        struct pam_conv *conv;
        
        msg.msg_style = PAM_ERROR_MSG;
        msg.msg = failure_msg;
        msgs[0] = &msg;
        
        // Get conversation function and display message
        retval = pam_get_item(pamh, PAM_CONV, (const void**)&conv);
        if (retval == PAM_SUCCESS && conv && conv->conv) {
            conv->conv(1, msgs, &resp, conv->appdata_ptr);
            if (resp) {
                if (resp[0].resp) {
                    free(resp[0].resp);
                }
                free(resp);
            }
        }
        
        syslog(LOG_AUTH | LOG_WARNING, "pam_smartcard_atr: Smart card authentication failed for user %s (ATR: %s)", username, card_atr);
        free(card_atr);
        return PAM_AUTH_ERR;
    }
}

// PAM credential setting (required for authentication)
PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

// PAM account management (required)
PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

// PAM session management (stub)
PAM_EXTERN int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

// PAM password management (stub)
PAM_EXTERN int pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_IGNORE;
}