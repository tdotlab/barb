// Scenario tests for the agent: a scripted model and a fake world (see harness.h).
// Every request the agent would send is checked against the rules the API enforces.
#include "harness.h"

static void test_chat(void) {
    world_t* w = w_new(1);
    w_reply_text(w, "Hello there.");
    w_input(w, "hi");
    CHECK_INT(w->nbodies, 1);
    CHECK(w_said(w, "Hello there."));
    CHECK(w_said(w, "[tokens: 100 in, 20 out]"));
    CHECK(strstr(w->bodies[0], "You are Barb"));
    CHECK(strstr(w->bodies[0], "Working directory: /work"));
    CHECK(strstr(w->bodies[0], "\"name\":\"read_file\""));
    CHECK(strstr(w->bodies[0], "\"model\":\"claude-test\""));
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "hi");
    free(t);
    CHECK_INT(w->nbodies, 1);        // nothing more is asked of the model: it is idle
    CHECK(!w->done);
    w_event(w, EV_EOF, 0, 0, 0, 0);
    CHECK(w->done && w->exit_code == 0);
    w_free(w);
}

static void test_read_file(void) {
    world_t* w = w_new(1);
    w_put_file(w, "a.txt", "one\ntwo\nthree\n");
    w_put_file(w, "ten.txt", "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");
    w_put_file(w, "empty.txt", "");
    w_put_file(w, "dir", 0);
    w_put_file(w, "crlf.txt", "a\r\nb\r\n");
    w_put_file_n(w, "bin.dat", "ab\0cd", 5);
    char longline[3000];
    memset(longline, 'x', 2500); longline[2500] = 0;
    w_put_file(w, "long.txt", longline);

    w_reply_tools(w, TOOL("t1", "read_file", J({"path":"./a.txt"})) ","
                     TOOL("t2", "read_file", J({"path":"ten.txt","offset":3,"limit":2})) ","
                     TOOL("t3", "read_file", J({"path":"empty.txt"})) ","
                     TOOL("t4", "read_file", J({"path":"nope.txt"})) ","
                     TOOL("t5", "read_file", J({"path":"dir"})) ","
                     TOOL("t6", "read_file", J({"path":"crlf.txt"})) ","
                     TOOL("t7", "read_file", J({"path":"bin.dat"})) ","
                     TOOL("t8", "read_file", J({"path":"long.txt"})) ","
                     TOOL("t9", "read_file", J({"path":"ten.txt","offset":11})));
    w_reply_text(w, "ok");
    w_input(w, "read things");
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT(w, "t1", "     1\tone\n     2\ttwo\n     3\tthree\n");
    CHECK_RESULT(w, "t2", "     3\tl3\n     4\tl4\n\n(showing lines 3-4 of 10; call read_file again with offset 5 to continue)");
    CHECK_RESULT(w, "t3", "(the file is empty)");
    CHECK_RESULT_HAS(w, "t4", "No such file: nope.txt");
    CHECK_RESULT_HAS(w, "t5", "is a directory");
    CHECK_RESULT(w, "t6", "     1\ta\n     2\tb\n");
    CHECK_RESULT_HAS(w, "t7", "looks like a binary file");
    CHECK_RESULT_HAS(w, "t8", "[line cut: it is 2500 bytes long]");
    CHECK_RESULT_HAS(w, "t9", "offset 11 is past the end of the file, which has 10 lines");
    int err = 0;
    free(w_result(w, "t4", &err));
    CHECK(err);
    err = 1;
    free(w_result(w, "t1", &err));
    CHECK(!err);
    w_free(w);
}

static void test_input_errors(void) {
    world_t* w = w_new(1);
    w_reply_tools(w, TOOL("t1", "read_file", "[]") "," TOOL("t2", "read_file", "{}") ","
                     TOOL("t3", "read_file", J({"path":5})) "," TOOL("t4", "read_file", J({"path":"a\u0000b"})) ","
                     TOOL("t5", "frobnicate", "{}") "," TOOL("t6", "read_file", J({"path":""})) ","
                     TOOL("t7", "read_file", J({"path":"x","offset":"two"})) "," TOOL("t8", "bash", J({"command":"  "})));
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT_HAS(w, "t1", "must be a JSON object");
    CHECK_RESULT_HAS(w, "t2", "Missing required argument 'path' (a string)");
    CHECK_RESULT_HAS(w, "t3", "Argument 'path' must be a string");
    CHECK_RESULT_HAS(w, "t4", "without NUL characters");
    CHECK_RESULT_HAS(w, "t5", "There is no tool named 'frobnicate'");
    CHECK_RESULT_HAS(w, "t6", "non-empty file path");
    CHECK_RESULT_HAS(w, "t7", "Argument 'offset' must be an integer");
    CHECK_RESULT_HAS(w, "t8", "The command is empty");
    w_free(w);
}

static void test_edit(void) {
    world_t* w = w_new(1);
    w_put_file(w, "src/a.c", "int a;\nint b;\nint a2;\nreturn 0;\n");
    w_put_file(w, "dup.c", "x = 1;\ny = 2;\nx = 1;\n");
    w_put_file(w, "crlf.c", "p\r\nq\r\n");
    w_reply_tools(w, TOOL("r", "read_file", J({"path":"src//a.c"})) "," TOOL("r2", "read_file", J({"path":"dup.c"})) ","
                     TOOL("r3", "read_file", J({"path":"crlf.c"})));
    w_reply_tools(w,
        TOOL("e1", "edit_file", J({"path":"./src/a.c","old_string":"int b;","new_string":"long b;\nlong c;"})) ","
        TOOL("e2", "edit_file", J({"path":"dup.c","old_string":"x = 1;","new_string":"x = 9;"})) ","
        TOOL("e3", "edit_file", J({"path":"dup.c","old_string":"x = 1;","new_string":"x = 9;","replace_all":true})) ","
        TOOL("e4", "edit_file", J({"path":"src/a.c","old_string":"nothing like this","new_string":"z"})) ","
        TOOL("e5", "edit_file", J({"path":"src/a.c","old_string":"  return 0;","new_string":"z"})) ","
        TOOL("e6", "edit_file", J({"path":"crlf.c","old_string":"p\nq","new_string":"z"})) ","
        TOOL("e7", "edit_file", J({"path":"src/a.c","old_string":"int a;","new_string":"int a;"})) ","
        TOOL("e8", "edit_file", J({"path":"src/a.c","old_string":"","new_string":"z"})) ","
        TOOL("e9", "edit_file", J({"path":"missing.c","old_string":"a","new_string":"b"})) ","
        TOOL("e10", "edit_file", J({"path":"unread.c","old_string":"a","new_string":"b"})));
    w_put_file(w, "unread.c", "a\n");
    w_reply_text(w, "done");
    w_input(w, "edit things");
    CHECK_INT(w->nbodies, 3);
    CHECK_STR(str_z(w_file(w, "src/a.c")), "int a;\nlong b;\nlong c;\nint a2;\nreturn 0;\n");
    CHECK_RESULT_HAS(w, "e1", "Edited src/a.c: replaced 1 occurrence.");
    CHECK_RESULT_HAS(w, "e1", "     2\tlong b;\n     3\tlong c;");
    CHECK_RESULT_HAS(w, "e2", "old_string appears 2 times in dup.c (at lines 1, 3)");
    CHECK_RESULT_HAS(w, "e2", "set replace_all to true");
    CHECK_RESULT_HAS(w, "e3", "replaced 2 occurrences");
    CHECK_STR(str_z(w_file(w, "dup.c")), "x = 9;\ny = 2;\nx = 9;\n");
    CHECK_RESULT_HAS(w, "e4", "old_string was not found in src/a.c");
    CHECK_RESULT_HAS(w, "e5", "whitespace around it differs");
    CHECK_RESULT_HAS(w, "e6", "CRLF");
    CHECK_RESULT_HAS(w, "e7", "identical");
    CHECK_RESULT_HAS(w, "e8", "must not be empty");
    CHECK_RESULT_HAS(w, "e9", "No such file: missing.c");
    CHECK_RESULT_HAS(w, "e10", "You have not read unread.c");
    CHECK_STR(str_z(w_file(w, "unread.c")), "a\n");   // untouched
    w_free(w);
}

static void test_write(void) {
    world_t* w = w_new(1);
    w_put_file(w, "old.txt", "keep\n");
    w_put_file(w, "seen.txt", "was\n");
    w_reply_tools(w, TOOL("r", "read_file", J({"path":"seen.txt"})));
    w_reply_tools(w, TOOL("w1", "write_file", J({"path":"new/dir/n.txt","content":"hello\n"})) ","
                     TOOL("w2", "write_file", J({"path":"old.txt","content":"clobber"})) ","
                     TOOL("w3", "write_file", J({"path":"seen.txt","content":"now\n"})) ","
                     TOOL("w4", "write_file", J({"path":"e.txt","content":""})));
    w_reply_text(w, "ok");
    w_input(w, "write things");
    CHECK_RESULT(w, "w1", "Created new/dir/n.txt (6 bytes).");
    CHECK_STR(str_z(w_file(w, "new/dir/n.txt")), "hello\n");
    CHECK_RESULT_HAS(w, "w2", "already exists and you have not read it");
    CHECK_STR(str_z(w_file(w, "old.txt")), "keep\n");
    CHECK_RESULT(w, "w3", "Replaced seen.txt (4 bytes).");
    CHECK_STR(str_z(w_file(w, "seen.txt")), "now\n");
    CHECK_RESULT(w, "w4", "Created e.txt (0 bytes).");
    w_free(w);
}

static void test_permissions(void) {
    // Asking: yes, no, and yes-always.
    world_t* w = w_new(0);
    w->ask_answer = ANS_YES;
    w_put_file(w, "f.txt", "a\nb\n");
    w_reply_tools(w, TOOL("r", "read_file", J({"path":"f.txt"})));
    w_reply_tools(w, TOOL("e1", "edit_file", J({"path":"f.txt","old_string":"a","new_string":"A"})));
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK_INT(w->nasks, 1);                       // reading needs no permission; the edit did
    CHECK(strstr(w->asks[0], "edit f.txt (1 place)"));
    CHECK(strstr(w->asks[0], "- a"));
    CHECK(strstr(w->asks[0], "+ A"));
    CHECK_STR(str_z(w_file(w, "f.txt")), "A\nb\n");
    w_free(w);

    w = w_new(0);
    w->ask_answer = ANS_NO;
    w_put_file(w, "f.txt", "a\n");
    w_reply_tools(w, TOOL("r", "read_file", J({"path":"f.txt"})));
    w_reply_tools(w, TOOL("e1", "edit_file", J({"path":"f.txt","old_string":"a","new_string":"A"})) ","
                     TOOL("b1", "bash", J({"command":"rm -rf /"})));
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK_RESULT_HAS(w, "e1", "The user declined this action");
    CHECK_RESULT_HAS(w, "b1", "The user declined this action");
    CHECK_STR(str_z(w_file(w, "f.txt")), "a\n");
    CHECK_INT(w_last_op(w, ACT_SPAWN), 0);        // the command never ran
    w_free(w);

    w = w_new(0);
    w->ask_answer = ANS_ALWAYS;
    w->nprocs = 1; w->procs[0].match = "echo"; w->procs[0].out = "x\n";
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"echo one"})));
    w_reply_tools(w, TOOL("b2", "bash", J({"command":"echo two"})));
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK_INT(w->nasks, 1);                       // "always" was remembered: the second ran unasked
    CHECK_RESULT(w, "b2", "x\n[exit code: 0]");
    w_free(w);

    // A question left open is answered later.
    w = w_new(0);
    w->nprocs = 1; w->procs[0].match = "ls"; w->procs[0].out = "f\n";
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"ls","description":"list files"})));
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK_INT(w->nasks, 1);
    CHECK(strstr(w->asks[0], "run:\nls\n(list files)"));
    CHECK_INT(w->nbodies, 1);                      // waiting for the user, not the model
    w_event(w, EV_ANSWER, w_last_op(w, ACT_ASK), ANS_YES, 0, 0);
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT(w, "b1", "f\n[exit code: 0]");
    w_free(w);
}

static void test_bash(void) {
    world_t* w = w_new(1);
    w->nprocs = 7;
    w->procs[5] = (typeof(w->procs[5])){ "sleep 15", "", 0, PROC_TIMEDOUT };
    w->procs[6] = (typeof(w->procs[6])){ "sleep 16", "", 0, PROC_TIMEDOUT };
    w->procs[0] = (typeof(w->procs[0])){ "echo", "hi\n", 0, 0 };
    w->procs[1] = (typeof(w->procs[1])){ "false", "", 1, 0 };
    w->procs[2] = (typeof(w->procs[2])){ "sleep 999", "partial", 0, PROC_TIMEDOUT };
    w->procs[3] = (typeof(w->procs[3])){ "nonesuch", "", 0, PROC_FAILED };
    char* big = malloc(60001);
    for (int i = 0; i < 60000; i++) big[i] = (i % 80 == 79) ? '\n' : 'a' + i % 26;
    big[60000] = 0;
    w->procs[4] = (typeof(w->procs[4])){ "bigout", big, 0, 0 };
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"echo hi"})) "," TOOL("b2", "bash", J({"command":"false"})) ","
                     TOOL("b3", "bash", J({"command":"sleep 999","timeout_ms":2000})) ","
                     TOOL("b4", "bash", J({"command":"nonesuch"})) "," TOOL("b5", "bash", J({"command":"bigout"})) ","
                     TOOL("b6", "bash", J({"command":"sleep 15","timeout_ms":1500})) ","
                     TOOL("b7", "bash", J({"command":"sleep 16","timeout_ms":1000})));
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK_RESULT(w, "b1", "hi\n[exit code: 0]");
    CHECK_RESULT(w, "b2", "(no output)\n[exit code: 1]");
    CHECK_RESULT_HAS(w, "b3", "partial\n[killed: it ran longer than its limit of 2 seconds.");
    CHECK_RESULT_HAS(w, "b6", "[killed: it ran longer than its limit of 1.5 seconds.");   // not "1 seconds"
    CHECK_RESULT_HAS(w, "b7", "[killed: it ran longer than its limit of 1 second.");
    CHECK_RESULT_HAS(w, "b4", "The command could not be started: no such shell");
    CHECK_RESULT_HAS(w, "b5", "bytes omitted from the middle");
    char* r = w_result(w, "b5", 0);
    CHECK(strlen(r) < 31000);
    free(r);
    int e1 = 0, e2 = 0, e3 = 0;
    free(w_result(w, "b2", &e1)); free(w_result(w, "b3", &e2)); free(w_result(w, "b4", &e3));
    CHECK(!e1 && e2 && e3);                         // a failing command is not an error; a timeout is
    // The timeout asked for reached the platform, clamped.
    int found = 0;
    for (int i = 0; i < w->nops; i++) if (w->ops[i].kind == ACT_SPAWN && strstr(w->ops[i].a, "sleep 999")) { found = 1; CHECK_INT(w->ops[i].n, 2000); }
    CHECK(found);
    free(big);
    w_free(w);
}

static void test_background(void) {
    world_t* w = w_new(1);
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"make -j8","run_in_background":true})));
    w_reply_text(w, "Building; I will wait.");
    w_input(w, "build it");
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT_HAS(w, "b1", "Started background task");
    CHECK_RESULT_HAS(w, "b1", ".barb/tasks/");
    CHECK_RESULT_HAS(w, "b1", "You will be woken");
    int64_t task = w_last_op(w, ACT_SPAWN);
    op_t* o = w_op(w, task);
    CHECK(o && o->bg);
    CHECK(strstr(o->a, "make -j8"));
    CHECK(strstr(o->c, ".barb/tasks/") && strstr(o->c, ".log"));
    CHECK(w_said(w, "(waiting for task"));             // it tells the user it is asleep
    CHECK(!w->done);
    int before = w->nbodies;
    // Nothing happens, and no model call is made, until the task ends.
    w_event(w, EV_TIMER, 9999, 0, 0, 0);               // an unknown timer: ignored
    w_event(w, EV_PROC, 9999, 0, 0, "x");              // an unknown process: ignored
    CHECK_INT(w->nbodies, before);

    w_reply_text(w, "The build passed.");
    w_event(w, EV_EOF, 0, 0, 0, 0);                    // no more input; but a task is still running
    CHECK(!w->done);
    w_event(w, EV_PROC, task, 0, 0, "");
    CHECK_INT(w->nbodies, before + 1);                 // woken: one model call
    char* t = w_last_user_text(w);
    CHECK(strstr(t, "[barb] Background task"));
    CHECK(strstr(t, "finished with exit code 0"));
    CHECK(strstr(t, "make -j8"));
    free(t);
    CHECK(w_said(w, "The build passed."));
    CHECK(w->done && w->exit_code == 0);               // and now everything is finished
    w_free(w);

    // A task that fails, and one stopped with task_stop.
    w = w_new(1);
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"sleep 3600","run_in_background":true})));
    w_reply_tools(w, TOOL("s1", "task_stop", J({"task_id":999})) "," TOOL("s2", "task_stop", "{\"task_id\":2}"));
    w_reply_text(w, "stopped");
    w_input(w, "go");
    CHECK_RESULT_HAS(w, "s1", "No running background task has the number 999. Running now: ");
    int64_t id = w_last_op(w, ACT_SPAWN);
    char want[64];
    snprintf(want, sizeof want, "{\"task_id\":%lld}", (long long)id);
    (void)want;
    w_free(w);
}

static void test_wakeup(void) {
    world_t* w = w_new(1);
    w_reply_tools(w, TOOL("w1", "schedule_wakeup", J({"seconds":60,"note":"check the build"})) ","
                     TOOL("w2", "schedule_wakeup", J({"seconds":0})) ","
                     TOOL("w3", "schedule_wakeup", J({"seconds":99999})));
    w_reply_text(w, "Waiting.");
    w_input(w, "later");
    CHECK_RESULT_HAS(w, "w1", "you will be woken in 60 seconds");
    CHECK_RESULT_HAS(w, "w2", "between 1 and 86400");
    CHECK_RESULT_HAS(w, "w3", "between 1 and 86400");
    int64_t timer = w_last_op(w, ACT_TIMER);
    CHECK(timer != 0);
    CHECK_INT(w_op(w, timer)->n, 60000);
    int before = w->nbodies;
    w_reply_text(w, "Checked.");
    w_event(w, EV_TIMER, timer, 0, 0, 0);
    CHECK_INT(w->nbodies, before + 1);
    char* t = w_last_user_text(w);
    CHECK(strstr(t, "The timer you set has fired. Your note: check the build"));
    free(t);
    w_event(w, EV_TIMER, timer, 0, 0, 0);              // a second ring of the same timer does nothing
    CHECK_INT(w->nbodies, before + 1);
    w_free(w);

    // A loop of wake-ups cannot spend without limit: after max_turns model calls it stops.
    w = w_new_turns(1, 4);
    for (int i = 0; i < 4; i++)
        w_reply_tools(w, TOOL("w", "schedule_wakeup", J({"seconds":1})));
    w_reply_text(w, "back");                            // for after the user speaks again
    w_input(w, "start");
    for (int i = 0; i < 10 && w->nbodies < 10; i++) {
        int64_t t2 = w_last_op(w, ACT_TIMER);
        if (!t2) break;
        w_event(w, EV_TIMER, t2, 0, 0, 0);
    }
    CHECK_INT(w->nbodies, 4);
    CHECK(w_said(w, "Stopped after 4 model calls"));
    // The user speaking resets the count.
    w_input(w, "carry on");
    CHECK_INT(w->nbodies, 5);
    w_free(w);
}

static void test_interrupts(void) {
    // During a model request: cancelled, and what the user types next joins their first message.
    world_t* w = w_new(1);
    w->hold_http = 1;
    w_input(w, "first");
    int64_t req = w_last_op(w, ACT_HTTP);
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_op(w, req)->cancelled);
    CHECK(w_said(w, "Interrupted."));
    int said = w->nsays;
    w_event(w, EV_HTTP, req, 200, 0, w->nrep ? w->replies[0] : "{}");   // a late reply to a cancelled request
    CHECK_INT(w->nsays, said);
    w->hold_http = 0;
    w_reply_text(w, "ok");
    w_input(w, "second");
    char* t = w_last_user_text(w);
    // ...with a line saying it was interrupted, so the model does not take up the old request as if
    // nothing had happened.
    CHECK_STR(str_z(t), "first|[barb] The user interrupted you here, before you replied.|second");
    free(t);
    w_free(w);

    // During tool calls: every call is answered, and the model is not called.
    w = w_new(1);
    w->hold_proc = 1;
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"sleep 100"})) "," TOOL("b2", "bash", J({"command":"echo never"})));
    w_input(w, "go");
    CHECK_INT(w->nbodies, 1);
    int64_t spawn = w_last_op(w, ACT_SPAWN);
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_op(w, spawn)->cancelled);
    CHECK_INT(w->nbodies, 1);
    w_event(w, EV_PROC, spawn, 0, 0, "late");        // the killed process reports in: ignored
    CHECK_INT(w->nbodies, 1);
    w_reply_text(w, "ok");
    w_input(w, "never mind, do this instead");
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT_HAS(w, "b1", "Interrupted by the user");
    CHECK_RESULT_HAS(w, "b2", "Not run: the user interrupted");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "never mind, do this instead");
    free(t);
    w_free(w);

    // During a question to the user.
    w = w_new(0);
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"ls"})));
    w_input(w, "go");
    CHECK_INT(w->nasks, 1);
    int64_t q = w_last_op(w, ACT_ASK);
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_op(w, q)->cancelled);
    w_event(w, EV_ANSWER, q, ANS_YES, 0, 0);          // too late: ignored, the command does not run
    CHECK_INT(w_last_op(w, ACT_SPAWN), 0);
    w_free(w);

    // Twice at an idle prompt quits.
    w = w_new(1);
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(!w->done);
    CHECK(w_said(w, "Press Ctrl-C again"));
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w->done && w->exit_code == 130);
    w_free(w);
    // ...but not if something else happened in between.
    w = w_new(1);
    w_reply_text(w, "hi");
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    w_input(w, "hello");
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(!w->done);
    w_free(w);
}

static void test_api_errors(void) {
    // A transient error is retried with a delay, the same request is sent again, and then it works.
    world_t* w = w_new(1);
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}", 529);
    w_reply_text(w, "finally");
    w_input(w, "hi");
    CHECK_INT(w->nbodies, 1);
    CHECK(w_said(w, "API returned HTTP 529; retrying in 1s (attempt 1 of 5)"));
    int64_t timer = w_last_op(w, ACT_TIMER);
    CHECK_INT(w_op(w, timer)->n, 1000);
    w_event(w, EV_TIMER, timer, 0, 0, 0);
    CHECK_INT(w->nbodies, 2);
    CHECK_STR(str_z(w->bodies[1]), w->bodies[0]);     // exactly the same request
    CHECK(w_said(w, "finally"));
    w_free(w);

    // Retries run out: the delay doubles each time, and the last error is shown.
    w = w_new(1);
    for (int i = 0; i < 6; i++)
        w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}", 529);
    w_input(w, "hi");
    for (int i = 0; i < 5; i++) {
        int64_t t = w_last_op(w, ACT_TIMER);
        CHECK_INT(w_op(w, t)->n, 1000LL << i);
        w_event(w, EV_TIMER, t, 0, 0, 0);
    }
    CHECK_INT(w->nbodies, 6);
    CHECK(w_said(w, "API error (HTTP 529, overloaded_error): Overloaded"));
    // The conversation is still sound: the next message joins the unanswered one.
    w_reply_text(w, "ok");
    w_input(w, "again");
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "hi|again");
    free(t);
    w_free(w);

    // The request itself failing.
    w = w_new(1);
    w_reply(w, "", 0);
    w_reply_text(w, "back");
    w_input(w, "hi");
    CHECK(w_said(w, "request failed: could not connect; retrying in 1s"));
    w_event(w, EV_TIMER, w_last_op(w, ACT_TIMER), 0, 0, 0);
    CHECK(w_said(w, "back"));
    w_free(w);

    // Errors that are not worth retrying.
    w = w_new(1);
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}", 401);
    w_input(w, "hi");
    CHECK_INT(w->nbodies, 1);
    CHECK_INT(w_last_op(w, ACT_TIMER), 0);
    CHECK(w_said(w, "API error (HTTP 401, authentication_error): invalid x-api-key"));
    CHECK(w_said(w, "Check ANTHROPIC_API_KEY"));
    w_event(w, EV_EOF, 0, 0, 0, 0);
    CHECK(w->done && w->exit_code == 1);               // a failed run says so in its exit status
    w_free(w);

    w = w_new(1);
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long: 250000 tokens > 200000\"}}", 400);
    w_input(w, "hi");
    CHECK(w_said(w, "Use /clear to start over"));
    w_free(w);

    // A 200 that is not a message, and a gateway's HTML error page.
    w = w_new(1);
    w_reply(w, "{\"hello\":1}", 200);
    w_reply(w, "<html>bad gateway</html>", 502);
    w_input(w, "hi");
    CHECK(w_said(w, "unreadable reply"));
    w_free(w);
}

static void test_queued_and_turns(void) {
    // Typing while the model is busy: queued, and sent as the next message when the reply is in.
    world_t* w = w_new(1);
    w->hold_http = 1;
    w_input(w, "A");
    int64_t r1 = w_last_op(w, ACT_HTTP);
    w_input(w, "B");
    CHECK(w_said(w, "(queued"));
    CHECK_INT(w->nbodies, 1);
    char* ra = mk_reply("end_turn", "[{\"type\":\"text\",\"text\":\"answer to A\"}]");
    w_event(w, EV_HTTP, r1, 200, 0, ra);
    free(ra);
    CHECK_INT(w->nbodies, 2);
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "B");
    free(t);
    w_free(w);

    // Typing during tool calls: it follows the results, in the same message.
    w = w_new(1);
    w->hold_proc = 1;
    w_reply_tools(w, TOOL("b1", "bash", J({"command":"ls"})));
    w_input(w, "go");
    w_input(w, "also, be careful");
    int64_t sp = w_last_op(w, ACT_SPAWN);
    w_reply_text(w, "ok");
    w_event(w, EV_PROC, sp, 0, 0, "x\n");
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT(w, "b1", "x\n[exit code: 0]");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "also, be careful");
    free(t);
    w_free(w);

    // A reply cut off at max_tokens with a tool call in it: the call is not run, but it is answered.
    w = w_new(1);
    char* cut = mk_reply("max_tokens", "[{\"type\":\"text\",\"text\":\"writing\"}," TOOL("t1", "write_file", "{\"path\":\"big.txt\",\"content\":\"abc") "]");
    // The cut reply's input is complete JSON here (the API closes it); what matters is stop_reason.
    free(cut);
    cut = mk_reply("max_tokens", "[" TOOL("t1", "write_file", J({"path":"big.txt","content":"abc"})) "]");
    w_reply(w, cut, 200);
    free(cut);
    w_reply_text(w, "I will do it in parts.");
    w_input(w, "write a big file");
    CHECK_INT(w->nbodies, 2);
    CHECK_RESULT_HAS(w, "t1", "Not run: your reply was cut off");
    CHECK(!w_file(w, "big.txt"));
    w_free(w);

    // The turn limit with tool calls pending: results are kept, and the user can carry on.
    w = w_new_turns(1, 2);
    w_put_file(w, "a", "x\n");
    w_reply_tools(w, TOOL("t1", "read_file", J({"path":"a"})));
    w_reply_tools(w, TOOL("t2", "read_file", J({"path":"a"})));
    w_reply_text(w, "continuing");
    w_input(w, "loop");
    CHECK_INT(w->nbodies, 2);
    CHECK(w_said(w, "Stopped after 2 model calls"));
    w_input(w, "go on");
    CHECK_INT(w->nbodies, 3);
    CHECK_RESULT(w, "t2", "     1\tx\n");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "go on");
    free(t);
    w_free(w);
}

static void test_commands(void) {
    world_t* w = w_new(1);
    w_reply_text(w, "one");
    w_input(w, "hello");
    w_input(w, "/clear");
    CHECK(w_said(w, "Conversation cleared"));
    w_reply_text(w, "two");
    w_input(w, "fresh start");
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "fresh start");
    free(t);
    CHECK(!strstr(w->bodies[1], "hello"));
    w_input(w, "/help");
    CHECK(w_said(w, "/clear"));
    w_input(w, "/bogus");
    CHECK(w_said(w, "Unknown command /bogus"));
    w_reply_text(w, "path question answered");
    int n = w->nbodies;
    w_input(w, "/etc/hosts is broken");                // not a command: it is a question
    CHECK_INT(w->nbodies, n + 1);
    w_input(w, "   ");                                  // nothing
    CHECK_INT(w->nbodies, n + 1);
    w_input(w, "/exit");
    CHECK(w->done && w->exit_code == 0);
    w_free(w);
}

static void test_stale_events(void) {
    // A reply that is not for the request in flight is ignored; the real one still works.
    world_t* w = w_new(1);
    w->hold_http = 1;
    w_input(w, "hi");
    int64_t real = w_last_op(w, ACT_HTTP);
    char* wrong = mk_reply("end_turn", "[{\"type\":\"text\",\"text\":\"from nowhere\"}]");
    w_event(w, EV_HTTP, real + 1000, 200, 0, wrong);
    CHECK(!w_said(w, "from nowhere"));
    w_event(w, EV_HTTP, real, 200, 0, wrong);
    CHECK(w_said(w, "from nowhere"));
    free(wrong);
    w_free(w);

    w = w_new(1);
    w_reply_text(w, "ok");
    w_event(w, EV_HTTP, 12345, 200, 0, "{}");
    w_event(w, EV_FILE, 12345, FS_OK, 0, "x");
    w_event(w, EV_ANSWER, 12345, ANS_YES, 0, 0);
    w_event(w, EV_PROC, 12345, 0, 0, "x");
    w_event(w, EV_TIMER, 12345, 0, 0, 0);
    CHECK_INT(w->nbodies, 0);
    CHECK_INT(w->nsays, 0);
    w_input(w, "hi");
    CHECK_INT(w->nbodies, 1);
    w_free(w);
}

// Chaos: random events in random order, with every request checked against the API's rules.
// The agent may do whatever it likes with nonsense, except break the conversation or crash.
static unsigned rnd_state;
static unsigned rnd(unsigned n) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 8) % n; }

static void random_reply(world_t* w) {
    char c[1024];
    switch (rnd(12)) {
    case 0: case 1: case 2: w_reply_text(w, "Some text."); break;
    case 3: w_reply_tools(w, TOOL("a", "read_file", J({"path":"f.txt"}))); break;
    case 4: w_reply_tools(w, TOOL("a", "edit_file", J({"path":"f.txt","old_string":"x","new_string":"y"})) ","
                             TOOL("b", "bash", J({"command":"echo hi"}))); break;
    case 5: w_reply_tools(w, TOOL("a", "write_file", J({"path":"f.txt","content":"x"}))); break;
    case 6: w_reply_tools(w, TOOL("a", "bash", J({"command":"make","run_in_background":true})) ","
                             TOOL("b", "schedule_wakeup", J({"seconds":30,"note":"look"}))); break;
    case 7: w_reply_tools(w, TOOL("a", "task_stop", J({"task_id":3})) "," TOOL("b", "nonesuch", "{}")); break;
    case 8: snprintf(c, sizeof c, "[{\"type\":\"text\",\"text\":\"cut\"},%s]", TOOL("a", "bash", J({"command":"x"})));
            { char* r = mk_reply("max_tokens", c); w_reply(w, r, 200); free(r); } break;
    case 9: w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"busy\"}}", 529); break;
    case 10: w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"bad\"}}", 400); break;
    default: snprintf(c, sizeof c, "[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"after thinking\"}]");
            { char* r = mk_reply("end_turn", c); w_reply(w, r, 200); free(r); } break;
    }
}

// The body of the last request, as a C string the test can search.
static const char* last_body(world_t* w) { return w->nbodies ? w->bodies[w->nbodies - 1] : ""; }

static int count_reads(world_t* w, const char* path) {
    int n = 0;
    for (int i = 0; i < w->nops; i++) if (w->ops[i].kind == ACT_READ && !strcmp(w->ops[i].a, path)) n++;
    return n;
}

static void test_instructions(void) {
    // AGENTS.md, with an import, goes into the system prompt before the first request.
    world_t* w = w_new_cfg(1, 0, 1);
    w_put_file(w, "AGENTS.md", "Be terse.\n@docs/STYLE.md\nThe end.\n");
    w_put_file(w, "docs/STYLE.md", "Use tabs.");
    CHECK(!agent_busy(w->g));
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK_INT(count_reads(w, "AGENTS.md"), 1);
    CHECK_INT(count_reads(w, "docs/STYLE.md"), 1);
    CHECK_INT(count_reads(w, "CLAUDE.md"), 0);
    CHECK_INT(w->nbodies, 1);
    const char* b = last_body(w);
    CHECK(strstr(b, "Project instructions") != 0);
    CHECK(strstr(b, "from AGENTS.md in the working directory (with the files it imports), in full below") != 0);
    CHECK(strstr(b, "Be terse.\\nUse tabs.\\nThe end.") != 0);   // the import line replaced, in order
    CHECK(strstr(b, "@docs/STYLE.md") == 0);
    CHECK(w_said(w, "(following the project's instructions in AGENTS.md and docs/STYLE.md)"));
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "hi");
    free(t);
    CHECK(!agent_busy(w->g));
    // Read once per session, not per message; /clear keeps them.
    w_reply_text(w, "ok");
    w_input(w, "/clear");
    w_input(w, "again");
    CHECK_INT(count_reads(w, "AGENTS.md"), 1);
    CHECK_INT(w->nbodies, 2);
    CHECK(strstr(last_body(w), "Be terse.") != 0);
    w_free(w);

    // No AGENTS.md: CLAUDE.md, whose import of the missing AGENTS.md is marked, not dropped silently.
    w = w_new_cfg(1, 0, 1);
    w_put_file(w, "CLAUDE.md", "@AGENTS.md\nAlso: no emojis.\n");
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK_INT(count_reads(w, "AGENTS.md"), 2);
    CHECK_INT(count_reads(w, "CLAUDE.md"), 1);
    b = last_body(w);
    CHECK(strstr(b, "from CLAUDE.md in the working directory") != 0);
    CHECK(strstr(b, "@AGENTS.md  [barb: this file could not be read]") != 0);
    CHECK(strstr(b, "Also: no emojis.") != 0);
    w_free(w);

    // Neither file: nothing added, and the request goes out as before.
    w = w_new_cfg(1, 0, 1);
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK_INT(w->nbodies, 1);
    CHECK(strstr(last_body(w), "Project instructions") == 0);
    CHECK(!w_said(w, "following the project"));
    w_free(w);

    // An empty AGENTS.md counts as none; a directory called AGENTS.md too.
    w = w_new_cfg(1, 0, 1);
    w_put_file(w, "AGENTS.md", "  \n\n");
    w_put_file(w, "CLAUDE.md", "From Claude.");
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK(strstr(last_body(w), "From Claude.") != 0);
    w_free(w);

    // Turned off: no reads at all.
    w = w_new_cfg(1, 0, 0);
    w_put_file(w, "AGENTS.md", "Be terse.");
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK_INT(count_reads(w, "AGENTS.md"), 0);
    CHECK(strstr(last_body(w), "Be terse.") == 0);
    w_free(w);

    // Imports: at most eight, each once, and a file does not import itself.
    w = w_new_cfg(1, 0, 1);
    w_put_file(w, "AGENTS.md", "@AGENTS.md\n@a\n@b\n@a\n@c\n@d\n@e\n@f\n@g\n@h\n@i\n@ not an import\nx @a\n");
    const char* names[] = { "a", "b", "c", "d", "e", "f", "g", "h", "i" };
    for (int i = 0; i < 9; i++) w_put_file(w, names[i], names[i]);
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK_INT(count_reads(w, "AGENTS.md"), 1);
    CHECK_INT(count_reads(w, "a"), 1);
    CHECK_INT(count_reads(w, "h"), 1);
    CHECK_INT(count_reads(w, "i"), 0);
    b = last_body(w);
    CHECK(strstr(b, "@i  [barb") == 0 && strstr(b, "\\n@i\\n") != 0);   // the ninth stays as written
    CHECK(strstr(b, "@ not an import") != 0 && strstr(b, "x @a") != 0);
    w_free(w);

    // Too long: cut at 64 KB, and said so.
    w = w_new_cfg(1, 0, 1);
    char* big = malloc(70001);
    memset(big, 'k', 70000);
    big[70000] = 0;
    w_put_file(w, "AGENTS.md", big);
    free(big);
    w_reply_text(w, "ok");
    w_input(w, "hi");
    CHECK(strstr(last_body(w), "the instructions were cut here") != 0);
    w_free(w);

    // While they load, the agent is busy. The first message goes out once they are read, and one
    // typed meanwhile waits for the reply, as it would behind any request: the turns come out the
    // same as without instructions.
    w = w_new_cfg(1, 0, 1);
    w_put_file(w, "AGENTS.md", "Be terse.");
    w->hold_fs = 1;
    w_input(w, "alpha-msg");
    CHECK(agent_busy(w->g));
    w_input(w, "beta-msg");
    CHECK(w_said(w, "(queued"));
    CHECK_INT(w->nbodies, 0);
    w->hold_fs = 0;
    w_reply_text(w, "one");
    w_reply_text(w, "two");
    w_event(w, EV_FILE, w_last_op(w, ACT_READ), FS_OK, 0, "Be terse.");
    CHECK_INT(w->nbodies, 2);
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "beta-msg");
    free(t);
    CHECK(strstr(w->bodies[0], "\"alpha-msg\"") != 0 && strstr(w->bodies[0], "beta-msg") == 0);
    w_free(w);

    // Ctrl-C while they load: drop what was typed, carry on without them, and do not try again.
    w = w_new_cfg(1, 0, 1);
    w->hold_fs = 1;
    w_input(w, "first");
    w_input(w, "also-typed");
    int64_t rd = w_last_op(w, ACT_READ);
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_op(w, rd)->cancelled);
    CHECK(!agent_busy(w->g));
    CHECK_INT(w->nbodies, 0);
    w_event(w, EV_FILE, rd, FS_OK, 0, "LATE-READ-TEXT");   // the cancelled read answers anyway: ignored
    CHECK_INT(w->nbodies, 0);
    w->hold_fs = 0;
    w_reply_text(w, "ok");
    w_input(w, "second");
    CHECK_INT(w->nbodies, 1);
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "second");
    free(t);
    CHECK(strstr(last_body(w), "LATE-READ-TEXT") == 0);
    CHECK_INT(w->nops, 2);   // the one read, then the request: no second attempt
    w_free(w);

    // -p: the prompt then end of input; the instructions still come first.
    w = w_new_cfg(1, 0, 1);
    w_put_file(w, "AGENTS.md", "Be terse.");
    w_reply_text(w, "done");
    w_input(w, "task");
    w_event(w, EV_EOF, 0, 0, 0, 0);
    CHECK(w->done);
    CHECK_INT(w->exit_code, 0);
    CHECK(strstr(last_body(w), "Be terse.") != 0);
    w_free(w);
}

static void test_busy(void) {
    world_t* w = w_new(0);
    CHECK(!agent_busy(w->g));
    w->hold_http = 1;
    w_input(w, "hi");
    CHECK(agent_busy(w->g));                         // asking the model
    w->hold_http = 0;
    w_reply_tools(w, TOOL("t1", "bash", J({"command":"echo hi"})));
    w_event(w, EV_HTTP, w_last_op(w, ACT_HTTP), 200, 0, w->replies[w->irep++]);
    CHECK(agent_busy(w->g));                         // asking the user about a tool call
    w_reply_text(w, "ok");
    w_event(w, EV_ANSWER, w_last_op(w, ACT_ASK), ANS_YES, 0, 0);
    CHECK(!agent_busy(w->g));                        // done: the prompt may show
    w_reply_tools(w, TOOL("t2", "bash", J({"command":"sleep 9","run_in_background":true})));
    w_reply_text(w, "waiting");
    w->ask_answer = ANS_YES;
    w_input(w, "start it");
    CHECK(!agent_busy(w->g));                        // a background task is not the agent being busy
    w_free(w);
}

static void test_random_events(void) {
    long total_requests = 0;
    h_quiet = 1;
    for (unsigned seed = 1; seed <= 400; seed++) {
        rnd_state = seed * 7919u;
        world_t* w = w_new_cfg(rnd(2), 1 + (int)rnd(8), rnd(2));
        w_put_file(w, "f.txt", "x\nx\n");
        if (rnd(2)) w_put_file(w, "AGENTS.md", "Rules.\n@f.txt\n@gone\n");
        w->hold_http = (int)rnd(3) == 0;
        w->hold_proc = (int)rnd(3) == 0;
        w->ask_answer = (int)rnd(4) - 1;               // -1 leaves questions open
        w->nprocs = 1; w->procs[0].match = "echo"; w->procs[0].out = "hi\n";
        for (int i = 0; i < 20; i++) random_reply(w);
        for (int step = 0; step < 60 && !w->done; step++) {
            switch (rnd(10)) {
            case 0: case 1: w_input(w, "a message"); break;
            case 2: w_event(w, EV_INTERRUPT, 0, 0, 0, 0); break;
            case 3: if (w->nops) { op_t* o = &w->ops[rnd((unsigned)w->nops)]; w_event(w, EV_PROC, o->id, (int)rnd(3), (int)rnd(8), "out"); } break;
            case 4: if (w->nops) { op_t* o = &w->ops[rnd((unsigned)w->nops)]; w_event(w, EV_TIMER, o->id, 0, 0, 0); } break;
            case 5: if (w->nops) { op_t* o = &w->ops[rnd((unsigned)w->nops)]; w_event(w, EV_ANSWER, o->id, (int)rnd(3), 0, 0); } break;
            case 6: if (w->nops) { op_t* o = &w->ops[rnd((unsigned)w->nops)]; w_event(w, EV_FILE, o->id, (int)rnd(6), 0, "x\n"); } break;
            case 7: {   // a reply, to a real request or to a stale one
                int64_t id = w->nops && rnd(2) ? w->ops[rnd((unsigned)w->nops)].id : w_last_op(w, ACT_HTTP);
                if (w->irep < w->nrep) { w_event(w, EV_HTTP, id, w->status[w->irep], 0, w->replies[w->irep]); w->irep++; }
                break; }
            case 8: w->hold_http = 0; w->hold_proc = (int)rnd(2); w_input(w, "/clear"); break;
            default: w_input(w, "go on"); break;
            }
        }
        total_requests += w->nbodies;
        if (w->unexpected_requests) w->unexpected_requests = 0;
        w_free(w);
    }
    h_quiet = 0;
    CHECK(total_requests > 400);       // the chaos did get as far as asking the model, a lot
    CHECK_INT(mem_live, 0);
}

// A conversation that touches everything, used to try memory running out at every point.
static void scenario(world_t* w) {
    w_put_file(w, "a.txt", "one\ntwo\n");
    w_reply_tools(w, TOOL("t1", "read_file", J({"path":"a.txt"})) "," TOOL("t2", "bash", J({"command":"echo hi","run_in_background":true})));
    w_reply_tools(w, TOOL("t3", "edit_file", J({"path":"a.txt","old_string":"two","new_string":"2"})) ","
                     TOOL("t4", "write_file", J({"path":"b.txt","content":"b"})) "," TOOL("t5", "schedule_wakeup", J({"seconds":5})));
    w_reply_text(w, "waiting");
    w_reply_text(w, "task done");
    w_input(w, "do everything");
    int64_t sp = w_last_op(w, ACT_SPAWN);
    if (sp && !w->done) w_event(w, EV_PROC, sp, 0, 0, "");
    w_event(w, EV_EOF, 0, 0, 0, 0);
}

static void test_out_of_memory(void) {
    int clean = 0, fatal = 0, never_started = 0;
    // Twice: once with every allocation refused from the k-th on, once with only the k-th refused
    // (which is where a failure that is quietly shrugged off would go unnoticed).
    for (int once = 0; once <= 1; once++)
    for (long k = 0; k < 600; k++) {
        mem_fail_once = once;
        mem_count = 0;
        mem_refused = 0;
        mem_fail_after = k;
        world_t* w = w_new_cfg(1, 0, 1);
        if (!w->g) { never_started++; w_free(w); continue; }
        w_put_file(w, "AGENTS.md", "Rules.\n@a.txt\n");
        scenario(w);
        if (w->done && w->exit_code == 1) fatal++;
        else clean++;
        // The rule: a refused allocation is never shrugged off. If one was refused, the agent said
        // so and stopped; if none was, it carried on as normal (here: waiting for its timer).
        if (mem_refused) { CHECK(w->done && w->exit_code == 1 && w_said(w, "Out of memory")); }
        else { CHECK(!w->done && !w_said(w, "Out of memory")); }
        mem_fail_after = -1;
        w_free(w);
        if (mem_live != 0) { printf("leak after failing allocation %ld: %ld live\n", k, mem_live); mem_live = 0; checks_failed++; }
        checks_run++;
    }
    mem_fail_after = -1;
    mem_fail_once = 0;
    CHECK(fatal > 0);          // running out of memory is reported and stops the agent...
    CHECK(clean > 0);          // ...and with enough memory the scenario completes
    CHECK(never_started > 0);
    CHECK_INT(mem_live, 0);
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    test_chat();
    test_read_file();
    test_input_errors();
    test_edit();
    test_write();
    test_permissions();
    test_bash();
    test_background();
    test_wakeup();
    test_interrupts();
    test_instructions();
    test_busy();
    test_api_errors();
    test_queued_and_turns();
    test_commands();
    test_stale_events();
    test_random_events();
    test_out_of_memory();
    return checks_report("test_agent");
}
