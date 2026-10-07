/* Check ordering and fail-closed behavior without starting host services or
 * modifying /run/user on the machine running this test. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#define np_path_exists test_path_exists
#define np_mkdir_p test_mkdir
#define np_run test_run
#define chown test_chown
#define chmod test_chmod
#define main session_main
#include "nativepipe-session.c"
#undef main

static void run_volume_script(const char *state, const char *counter, const char *wpctl,
                               const char *search_path, bool no_sink) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(setenv("XDG_STATE_HOME", state, 1) == 0);
        assert(setenv("NP_VOLUME_TEST_COUNTER", counter, 1) == 0);
        assert(setenv("NP_VOLUME_TEST_NO_SINK", no_sink ? "1" : "0", 1) == 0);
        assert(setenv("PATH", search_path, 1) == 0);
        execl("/bin/sh", "sh", "-c", default_volume_script, "sh", wpctl, (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned volume_calls(const char *path) {
    unsigned calls = 0;
    FILE *file = fopen(path, "r");
    assert(file && fscanf(file, "%u", &calls) == 1);
    assert(fclose(file) == 0);
    return calls;
}

static void test_default_volume(void) {
    char directory[] = "/tmp/nativepipe-default-volume.XXXXXX";
    assert(mkdtemp(directory));
    char state[256], counter[256], wpctl[256], sleep_path[256], marker[256], marker_directory[256], search_path[512];
    assert(snprintf(state, sizeof(state), "%s/state", directory) < (int)sizeof(state));
    assert(snprintf(counter, sizeof(counter), "%s/calls", directory) < (int)sizeof(counter));
    assert(snprintf(wpctl, sizeof(wpctl), "%s/wpctl", directory) < (int)sizeof(wpctl));
    assert(snprintf(sleep_path, sizeof(sleep_path), "%s/sleep", directory) < (int)sizeof(sleep_path));
    assert(snprintf(marker_directory, sizeof(marker_directory), "%s/nativepipe", state) < (int)sizeof(marker_directory));
    assert(snprintf(marker, sizeof(marker), "%s/default-volume-v1", marker_directory) < (int)sizeof(marker));
    assert(snprintf(search_path, sizeof(search_path), "%s:/usr/bin:/bin", directory) < (int)sizeof(search_path));
    static const char fake_wpctl[] =
        "#!/bin/sh\n"
        "[ \"$#\" -eq 3 ] && [ \"$1\" = set-volume ] && [ \"$2\" = @DEFAULT_AUDIO_SINK@ ] && [ \"$3\" = 1.0 ] || exit 99\n"
        "count=0\n"
        "[ ! -f \"$NP_VOLUME_TEST_COUNTER\" ] || read -r count < \"$NP_VOLUME_TEST_COUNTER\"\n"
        "count=$((count + 1))\n"
        "printf '%s\\n' \"$count\" > \"$NP_VOLUME_TEST_COUNTER\"\n"
        "[ \"$NP_VOLUME_TEST_NO_SINK\" != 1 ] && [ \"$count\" -ge 3 ]\n";
    static const char fake_sleep[] = "#!/bin/sh\nexit 0\n";
    assert(np_write_file(wpctl, fake_wpctl, sizeof(fake_wpctl) - 1, 0755) == 0);
    assert(np_write_file(sleep_path, fake_sleep, sizeof(fake_sleep) - 1, 0755) == 0);
    run_volume_script(state, counter, wpctl, search_path, false);
    assert(volume_calls(counter) == 3 && access(marker, F_OK) == 0);
    run_volume_script(state, counter, wpctl, search_path, false);
    assert(volume_calls(counter) == 3); /* Do not overwrite a user's later volume. */
    assert(unlink(marker) == 0 && unlink(counter) == 0);
    run_volume_script(state, counter, wpctl, search_path, true);
    assert(volume_calls(counter) == 60 && access(marker, F_OK) != 0);
    run_volume_script(state, counter, wpctl, search_path, false);
    assert(volume_calls(counter) == 61 && access(marker, F_OK) == 0);
    assert(unlink(marker) == 0 && unlink(counter) == 0 && unlink(wpctl) == 0 && unlink(sleep_path) == 0);
    assert(rmdir(marker_directory) == 0 && rmdir(state) == 0 && rmdir(directory) == 0);
}

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
    test_default_volume();
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
    puts("session runtime: default-volume retries, user preference preservation, OpenRC and systemd ordering PASS");
    return 0;
}
