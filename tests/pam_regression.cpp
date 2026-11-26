// SPDX-License-Identifier: MIT
#include "base.hpp"

#include <security/pam_appl.h>
#include <iostream>

namespace {
int noConversation(int, const pam_message **, pam_response **, void *data)
{
    ++*static_cast<int *>(data);
    return PAM_CONV_ERR;
}
}

int main(int argc, char **argv)
{
    using namespace surface;
    if (argc != 2)
        return 2;
    try {
        std::string path = (fs::temp_directory_path() / "surface-ir-pam-test.XXXXXX").string();
        if (!mkdtemp(path.data()))
            syserror("Create PAM test directory");
        struct Fixture {
            fs::path location;
            ~Fixture() { std::error_code error; fs::remove_all(location, error); }
        } fixture{path};

        std::ofstream policy(fixture.location / "surface-ir-test");
        policy << "auth sufficient " << argv[1] << " quiet\n"
               << "auth required pam_deny.so\n";
        policy.close();
        if (!policy)
            throw std::runtime_error("Write isolated PAM policy");

        int prompts = 0;
        const pam_conv conversation{noConversation, &prompts};
        pam_handle_t *transaction = nullptr;
        int result = pam_start_confdir("surface-ir-test", nullptr, &conversation, path.c_str(), &transaction);
        if (result != PAM_SUCCESS)
            throw std::runtime_error("Start isolated PAM transaction");
        result = pam_set_item(transaction, PAM_RHOST, "remote-test.invalid");
        if (result == PAM_SUCCESS)
            result = pam_authenticate(transaction, PAM_SILENT);
        pam_end(transaction, result);
        if (result != PAM_AUTH_ERR || prompts != 0)
            throw std::runtime_error("Remote face authentication must fall through without prompting");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
