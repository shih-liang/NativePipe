/* Check ordering and fail-closed behavior without starting host services or
 * modifying /run/user on the machine running this test. */
#undef NDEBUG
#include <assert.h>
#define np_path_exists test_path_exists
#define np_mkdir_p test_mkdir
#define np_run test_run
#define chown test_chown
#define chmod test_chmod
#define main session_main
#include "nativepipe-session.c"
#undef main

static int systemd_present, fail_step, commands, directories, ownership;

int test_path_exists(const char *path) {
    assert(!strcmp(path, "/run/systemd/system"));
    return systemd_present;
}

int test_run(char *const args[]) {
    commands++;
    if (commands == 1) {
        assert(!strcmp(args[0], "/usr/bin/loginctl"));
        assert(!strcmp(args[1], "--no-ask-password"));
        assert(!strcmp(args[2], "enable-linger") && !strcmp(args[3], "installtest") && !args[4]);
    } else {
        assert(commands == 2 && !strcmp(args[0], "/usr/bin/systemctl"));
        assert(!strcmp(args[1], "--system") && !strcmp(args[2], "--no-ask-password"));
        assert(!strcmp(args[3], "start") && !strcmp(args[4], "user@1000.service") && !args[5]);
    }
    return commands == fail_step ? 1 : 0;
}

int test_mkdir(const char *path) {
    assert(!strcmp(path, "/run/user/1000"));
    assert(commands == (systemd_present ? 2 : 0));
    directories++;
    return 0;
}

int test_chown(const char *path, uid_t uid, gid_t gid) {
    assert(directories == 1 && !strcmp(path, "/run/user/1000") && uid == 1000 && gid == 1000);
    ownership++;
    return 0;
}

int test_chmod(const char *path, mode_t mode) {
    assert(ownership == 1 && !strcmp(path, "/run/user/1000") && mode == 0700);
    return 0;
}

int main(void) {
    struct passwd pw = {.pw_name = "installtest", .pw_uid = 1000, .pw_gid = 1000};
    char runtime[128];
    for (systemd_present = 0; systemd_present <= 1; systemd_present++) {
        for (fail_step = 0; fail_step <= (systemd_present ? 2 : 0); fail_step++) {
            commands = directories = ownership = 0;
            int result = setup_runtime(&pw, runtime, sizeof(runtime));
            if (fail_step) {
                assert(result < 0 && errno == EIO);
                assert(commands == fail_step && directories == 0 && ownership == 0);
            } else {
                assert(result == 0 && directories == 1 && ownership == 1);
                assert(!strcmp(runtime, "/run/user/1000"));
            }
        }
    }
    puts("session runtime: OpenRC, systemd ordering and preparation failures PASS");
    return 0;
}
