/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <cstdlib>
#include <cstring>

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int, int, const char **)
{
    const pam_conv *conversation = nullptr;
    if (pam_get_item(pamh, PAM_CONV, reinterpret_cast<const void **>(&conversation)) != PAM_SUCCESS || !conversation || !conversation->conv) {
        return PAM_SYSTEM_ERR;
    }

    pam_message message{PAM_PROMPT_ECHO_OFF, "Password:"};
    const pam_message *messagePointer = &message;
    pam_response *response = nullptr;
    const int conversationResult = conversation->conv(1, &messagePointer, &response, conversation->appdata_ptr);
    if (conversationResult != PAM_SUCCESS || !response) {
        std::free(response);
        return PAM_CONV_ERR;
    }

    const bool accepted = response[0].resp && std::strcmp(response[0].resp, "my_password") == 0;
    std::free(response[0].resp);
    std::free(response);
    if (accepted) {
        return PAM_SUCCESS;
    }

    pam_fail_delay(pamh, 150000);
    return PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *, int, int, const char **)
{
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *, int, int, const char **)
{
    return PAM_SUCCESS;
}
