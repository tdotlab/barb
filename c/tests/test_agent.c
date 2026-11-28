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
    CHECK_INT(w->style[w->nsays - 1], SAY_USAGE);
    CHECK_INT((int)w->usage, 120);
    CHECK(strstr(w->bodies[0], "You are Barb"));
    CHECK(strstr(w->bodies[0], "Working directory: /work"));
    CHECK(strstr(w->bodies[0], "\"name\":\"read_file\""));
    CHECK(strstr(w->bodies[0], "\"model\":\"claude-test\""));
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "hi");
    free(t);
    CHECK_INT(w->nbodies, 1);        // nothing more is asked of the model: it is idle
    CHECK(!w->done);
    w_reply(w, "{\"id\":\"m2\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],"
               "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":5,\"cache_creation_input_tokens\":7,"
               "\"cache_read_input_tokens\":1000,\"output_tokens\":30}}", 200);
    w_input(w, "again");
    CHECK(w_said(w, "[tokens: 1012 in (1000 cached), 30 out]"));
    CHECK_INT((int)w->usage, 1042);    // all of it is in the conversation the next request sends
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
    w->procs[5] = (proc_script_t){ "sleep 15", "", 0, PROC_TIMEDOUT };
    w->procs[6] = (proc_script_t){ "sleep 16", "", 0, PROC_TIMEDOUT };
    w->procs[0] = (proc_script_t){ "echo", "hi\n", 0, 0 };
    w->procs[1] = (proc_script_t){ "false", "", 1, 0 };
    w->procs[2] = (proc_script_t){ "sleep 999", "partial", 0, PROC_TIMEDOUT };
    w->procs[3] = (proc_script_t){ "nonesuch", "", 0, PROC_FAILED };
    char* big = malloc(60001);
    for (int i = 0; i < 60000; i++) big[i] = (i % 80 == 79) ? '\n' : 'a' + i % 26;
    big[60000] = 0;
    w->procs[4] = (proc_script_t){ "bigout", big, 0, 0 };
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
    CHECK(w_said(w, "The message does not fit the model's context window: send less at a time."));  // nothing to compact
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

static void w_reply_big(world_t* w, const char* text, int in);
static void w_reply_summary(world_t* w, const char* summary);

static void random_reply(world_t* w) {
    char c[1024];
    switch (rnd(16)) {
    case 12: w_reply_big(w, "big", 5000); return;
    case 13: w_reply_summary(w, "what happened"); return;
    case 14: w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long\"}}", 400); return;
    case 15: w_reply_text(w, "<analysis>no summary in this</analysis>"); return;
    }
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

// ---- compaction, verbose, saved sessions ----

static int w_said_as(world_t* w, const char* needle, int style) {
    for (int i = 0; i < w->nsays; i++) if (w->style[i] == style && strstr(w->says[i], needle)) return 1;
    return 0;
}
static int count_of(const char* hay, const char* needle) {
    int n = 0;
    for (const char* p = hay; (p = strstr(p, needle)) != 0; p += strlen(needle)) n++;
    return n;
}
static int n_said(world_t* w, const char* needle) {
    int n = 0;
    for (int i = 0; i < w->nsays; i++) if (strstr(w->says[i], needle)) n++;
    return n;
}
// A reply whose usage says the conversation is now `in` tokens big.
static void w_reply_big(world_t* w, const char* text, int in) {
    char c[2048], r[2400];
    snprintf(c, sizeof c, "[{\"type\":\"text\",\"text\":\"%s\"}]", text);
    snprintf(r, sizeof r, "{\"type\":\"message\",\"role\":\"assistant\",\"content\":%s,\"stop_reason\":\"end_turn\","
                          "\"usage\":{\"input_tokens\":%d,\"output_tokens\":20}}", c, in);
    w_reply(w, r, 200);
}
static void w_reply_summary(world_t* w, const char* summary) {
    char c[1024];
    snprintf(c, sizeof c, "<analysis>going over it</analysis>\\n<summary>%s</summary>", summary);
    w_reply_text(w, c);
}
#define INTERRUPTED_NOTE "[barb] The user interrupted you here, before you replied."
#define OVERFLOW_REPLY "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long: 250000 tokens > 200000\"}}"
#define OPTS(...) ((w_opts_t){ __VA_ARGS__ })

static void test_compact_parts(void) {
    static const struct { const char* in; int64_t want; } at[] = {
        { "off", 0 }, { "150000", 150000 }, { "200k", 200000 }, { "200K", 200000 }, { "1m", 1000000 },
        { "2M", 2000000 }, { " 3k ", 3000 }, { "1000", 1000 }, { "1k", 1000 }, { "999", -1 }, { "0", -1 },
        { "", -1 }, { "k", -1 }, { "12a", -1 }, { "-5", -1 }, { "1.5k", -1 }, { "Off", -1 },
        { "999999999999", 999999999999LL }, { "1234567890123", -1 }, { "5kk", -1 },
    };
    for (size_t i = 0; i < sizeof at / sizeof *at; i++) {
        if (parse_compact_at(str_z(at[i].in)) != at[i].want) printf("parse_compact_at(\"%s\")\n", at[i].in);
        CHECK_INT(parse_compact_at(str_z(at[i].in)), at[i].want);
    }
    CHECK(is_overflow(str_z("prompt is too long: 250000 tokens > 200000 maximum")));
    CHECK(is_overflow(str_z("This model's maximum context length is 8192 tokens")));
    CHECK(is_overflow(str_z("input exceeds the context window")));
    CHECK(!is_overflow(str_z("Overloaded")));
    CHECK(!is_overflow(str_z("")));

    arena_t a;
    arena_init(&a, &test_mem);
    str_t ins = compact_instruction(&a, str_z("  the failing test \n"));
    CHECK(str_starts(ins, str_z("[barb] Stop the task here. Do not call any tools")));
    CHECK(str_find(ins, str_z("<summary></summary>")) != STR_NONE);
    CHECK(str_find(ins, str_z("pay particular attention to this: the failing test")) != STR_NONE);
    CHECK(ins.p[ins.n - 1] == 't');                                // the focus is trimmed
    CHECK(str_find(compact_instruction(&a, str_z(" ")), str_z("particular attention")) == STR_NONE);

    CHECK_STR(compact_extract(&a, str_z("<analysis>a</analysis>\n<summary>\n S \n</summary>\ntrailing")), "S");
    CHECK_STR(compact_extract(&a, str_z("<summary>cut off here")), "cut off here");
    CHECK_STR(compact_extract(&a, str_z("before <analysis>x</analysis> after")), "before  after");
    CHECK_STR(compact_extract(&a, str_z("<analysis>cut off in the analysis")), "");
    CHECK_STR(compact_extract(&a, str_z("  just text  ")), "just text");
    CHECK_STR(compact_extract(&a, str_z("<summary></summary>")), "");
    CHECK_STR(compact_extract(&a, str_z("x <summary>in</summary> <summary>second</summary>")), "in");

    str_t m = compact_summary_message(&a, str_z("S1"));
    CHECK(str_starts(m, str_z(COMPACT_MARK)));
    CHECK(str_find(m, str_z("\n\n<summary>\nS1\n</summary>")) == m.n - strlen("\n\n<summary>\nS1\n</summary>"));

    // Trimmed for a request that has to fit: thinking goes, long things lose their middles, big
    // inputs become a note, and a message of only thinking keeps a place so the roles still alternate.
    transcript_t t = { 0, 0 }, out;
    char* big = malloc(20001);
    memset(big, 'x', 20000);
    big[20000] = 0;
    tr_add_text(&a, tr_user_msg(&a, &t), str_z(big));
    msg_t* as = tr_new_msg(&a, &t, 1);
    block_t* th = block_new(&a, BK_RAW);
    th->text = str_z("{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"s\"}");
    msg_add(as, th);
    block_t* other = block_new(&a, BK_RAW);
    other->text = str_z("{\"type\":\"server_tool_use\",\"id\":\"s1\"}");
    msg_add(as, other);
    block_t* tu = block_new(&a, BK_TOOL_USE);
    tu->id = str_z("t1"); tu->name = str_z("write_file");
    char* in = malloc(5100);
    snprintf(in, 5100, "{\"path\":\"a\",\"content\":\"%.*s\"}", 5000, big);
    tu->input = str_z(in);
    msg_add(as, tu);
    block_t* small = block_new(&a, BK_TOOL_USE);
    small->id = str_z("t2"); small->name = str_z("bash"); small->input = str_z("{\"command\":\"ls\"}");
    msg_add(as, small);
    msg_t* u = tr_user_msg(&a, &t);
    block_t* r1 = block_new(&a, BK_TOOL_RESULT);
    r1->id = str_z("t1"); r1->text = str_of(big, 5000); r1->is_error = 1;
    msg_add(u, r1);
    block_t* r2 = block_new(&a, BK_TOOL_RESULT);
    r2->id = str_z("t2"); r2->text = str_z("short");
    msg_add(u, r2);
    msg_t* only = tr_new_msg(&a, &t, 1);
    block_t* rt = block_new(&a, BK_RAW);
    rt->text = str_z("{\"type\":\"redacted_thinking\",\"data\":\"zz\"}");
    msg_add(only, rt);
    CHECK(compact_trimmed(&a, &t, &out));
    CHECK_INT(tr_count(&out), 4);
    const msg_t* q = out.first;
    CHECK(q->first->text.n < 16384 + 100 && q->first->text.n > 16000);
    CHECK(str_find(q->first->text, str_z("bytes omitted from the middle")) != STR_NONE);
    q = q->next;
    CHECK(q->first->kind == BK_RAW && str_find(q->first->text, str_z("server_tool_use")) != STR_NONE);
    CHECK(q->first->next->kind == BK_TOOL_USE);
    char note[128];
    snprintf(note, sizeof note, "{\"barb_note\":\"this input (%zu bytes) was left out of the summary request\"}", strlen(in));
    CHECK_STR(q->first->next->input, note);
    CHECK_STR(q->first->next->next->input, "{\"command\":\"ls\"}");
    q = q->next;
    CHECK(q->first->text.n < 4096 + 100 && q->first->is_error);
    CHECK_STR(q->first->next->text, "short");
    q = q->next;
    CHECK(q->assistant && q->first && q->first->kind == BK_TEXT);
    CHECK_STR(q->first->text, "[thinking left out]");
    CHECK(t.first->first->text.n == 20000);              // the conversation itself is untouched
    CHECK(t.first->next->first == th);
    free(big);
    free(in);
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

static void test_compaction(void) {
    // By hand: the summary request is the conversation as it is (same system prompt and tools)
    // with the instruction last; the summary then is the conversation, and nothing goes to the
    // model until the user says something.
    world_t* w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "the first answer");
    w_input(w, "hello there");
    w_reply_summary(w, "The user said hello.");
    w_input(w, "/compact keep the tests in view");
    CHECK(w_said(w, "(compacting the conversation)"));
    CHECK_INT(w->nbodies, 2);
    const char* sb = w->bodies[1];
    CHECK(strstr(sb, "hello there") && strstr(sb, "the first answer"));
    CHECK(strstr(sb, "[barb] Stop the task here") && strstr(sb, "pay particular attention to this: keep the tests in view"));
    CHECK(strstr(sb, "\"tools\"") && strstr(sb, "You are Barb"));
    CHECK(w_said(w, "(compacted: 2 messages became a summary)"));
    CHECK(w_said_as(w, "for the summary]", SAY_USAGE));
    CHECK(!w_said(w, "going over it"));                   // the analysis is not shown
    CHECK(!agent_busy(w->g));
    w_reply_text(w, "carrying on");
    w_input(w, "go on");
    CHECK_INT(w->nbodies, 3);
    char* t = w_last_user_text(w);
    CHECK(str_starts(str_z(t), str_z(COMPACT_MARK)));
    CHECK(strstr(t, "<summary>\nThe user said hello.\n</summary>|go on") != 0);
    free(t);
    CHECK(!strstr(w->bodies[2], "the first answer") && !strstr(w->bodies[2], "hello there"));
    CHECK(!strstr(w->bodies[2], "Stop the task here"));
    // And the summary is not compacted again by itself: there is nothing before it.
    w_input(w, "/compact at");
    CHECK(w_said(w, "Compaction: automatic compaction is off; the conversation takes about 120 now."));
    w_free(w);

    // Nothing to compact yet, and not while busy.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = DEFAULT_COMPACT_AT));
    w_input(w, "/compact");
    CHECK(w_said(w, "Nothing to compact yet."));
    CHECK_INT(w->nbodies, 0);
    w->hold_http = 1;
    w_input(w, "hi");
    w_input(w, "/compact");
    CHECK(w_said(w, "Busy: wait for the current step, or press Ctrl-C, before /compact."));
    CHECK_INT(w->nbodies, 1);
    w_free(w);

    // By itself: past the setting, before the next request. What the user just said is in the
    // summary request, and follows the summary word for word, once.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 1000));
    w_reply_big(w, "the first answer", 2000);
    w_input(w, "start");
    w_reply_summary(w, "S1");
    w_reply_text(w, "answer two");
    w_input(w, "second question");
    CHECK(w_said(w, "(compacting the conversation: it takes 2020 tokens, past the 1000 set for it)"));
    CHECK_INT(w->nbodies, 3);
    CHECK(strstr(w->bodies[1], "Stop the task here") && strstr(w->bodies[1], "second question"));
    t = w_last_user_text(w);
    CHECK(str_starts(str_z(t), str_z(COMPACT_MARK)));
    CHECK(strstr(t, "\n</summary>|second question") != 0);
    free(t);
    CHECK_INT(count_of(w->bodies[2], "second question"), 1);
    CHECK(!strstr(w->bodies[2], "the first answer"));
    CHECK(w_said(w, "answer two"));
    CHECK(!w_said(w, "the whole conversation is still in"));  // not saved: nowhere to point to
    w_free(w);

    // Under the setting: no compaction.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 3000));
    w_reply_big(w, "one", 2000);
    w_reply_text(w, "two");
    w_input(w, "a");
    w_input(w, "b");
    CHECK_INT(w->nbodies, 2);
    CHECK(!w_said(w, "compacting"));
    w_free(w);

    // By itself after a round of tool calls: the calls and their results go into the summary
    // together, and what goes out next is the summary alone, with no result left over.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 1000));
    w_put_file(w, "a.txt", "x\n");
    w_reply(w, "{\"type\":\"message\",\"content\":[" TOOL("r1", "read_file", J({"path":"a.txt"})) "],"
               "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":5000,\"output_tokens\":10}}", 200);
    w_reply_summary(w, "Read a.txt.");
    w_reply_text(w, "done");
    w_input(w, "read it");
    CHECK_INT(w->nbodies, 3);
    CHECK(strstr(w->bodies[1], "\"tool_result\"") != 0);
    CHECK(!strstr(w->bodies[2], "tool_result") && !strstr(w->bodies[2], "tool_use"));
    t = w_last_user_text(w);
    CHECK(str_starts(str_z(t), str_z(COMPACT_MARK)) && !strchr(t, '|'));
    free(t);
    CHECK(w_said(w, "done"));
    w_free(w);

    // The summary request fails: say so, go on without it, and do not try again on every request.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 1000));
    w_reply_big(w, "one", 2000);
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"nope\"}}", 400);
    w_reply_big(w, "two", 2500);
    w_reply_big(w, "three", 2600);
    w_input(w, "a");
    w_input(w, "b");
    CHECK(w_said(w, "Could not compact the conversation: API error (HTTP 400, invalid_request_error): nope\n"
                    "Going on without it; automatic compaction is paused until /compact works."));
    CHECK_INT(w->nbodies, 3);
    CHECK(strstr(w->bodies[2], "\"a\"") && strstr(w->bodies[2], "\"b\"") && !strstr(w->bodies[2], "Stop the task"));
    w_input(w, "c");
    CHECK_INT(w->nbodies, 4);
    CHECK_INT(n_said(w, "(compacting"), 1);
    w_input(w, "/compact at");
    CHECK(w_said(w, "Compaction: automatic once the conversation takes 1000 tokens (paused: the last try failed); "
                    "the conversation takes about 2620 now."));
    // Setting it again un-pauses it.
    w_input(w, "/compact at 1k");
    w_reply_summary(w, "S");
    w_reply_text(w, "four");
    w_input(w, "d");
    CHECK_INT(n_said(w, "(compacting"), 2);
    CHECK(w_said(w, "four"));
    w_free(w);

    // A manual one that works un-pauses it too; a manual one that fails leaves the conversation.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 1000));
    w_reply_big(w, "one", 2000);
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"nope\"}}", 400);
    w_reply_big(w, "two", 2500);
    w_input(w, "a");
    w_input(w, "b");
    w_reply_text(w, "<analysis>only this</analysis>");
    w_input(w, "/compact");
    CHECK(w_said(w, "Could not compact the conversation: the model wrote no summary\nIt is unchanged."));
    w_reply_summary(w, "S");
    w_input(w, "/compact");
    CHECK(w_said(w, "(compacted: 4 messages became a summary)"));
    w_input(w, "/compact at");
    CHECK(!w_said(w, "(paused"));
    w_free(w);

    // Cut off: with no summary it fails; with some it is used, and the user is told.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "a");
    char* cut = mk_reply("max_tokens", "[{\"type\":\"text\",\"text\":\"<analysis>long\"}]");
    w_reply(w, cut, 200);
    free(cut);
    w_input(w, "/compact");
    CHECK(w_said(w, "Could not compact the conversation: the summary was cut off at the output token limit"));
    cut = mk_reply("max_tokens", "[{\"type\":\"text\",\"text\":\"<summary>partial\"}]");
    w_reply(w, cut, 200);
    free(cut);
    w_input(w, "/compact");
    CHECK(w_said(w, "(compacted: 2 messages became a summary)"));
    CHECK(w_said(w, "(the summary was cut off at the output token limit: it may leave out the latest work)"));
    w_reply_text(w, "ok");
    w_input(w, "b");
    CHECK(strstr(last_body(w), "<summary>\\npartial\\n</summary>") != 0);
    w_free(w);

    // A summary request is retried like any other, the same request again.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "a");
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"busy\"}}", 529);
    w_reply_summary(w, "S");
    w_input(w, "/compact");
    w_event(w, EV_TIMER, w_last_op(w, ACT_TIMER), 0, 0, 0);
    CHECK_INT(w->nbodies, 3);
    CHECK_STR(str_z(w->bodies[2]), w->bodies[1]);
    CHECK(w_said(w, "(compacted: 2 messages became a summary)"));
    w_free(w);

    // The request no longer fits: compact, trimmed, and send it again.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = DEFAULT_COMPACT_AT));
    char* th = mk_reply("end_turn", "[{\"type\":\"thinking\",\"thinking\":\"deep\",\"signature\":\"sig\"},{\"type\":\"text\",\"text\":\"one\"}]");
    w_reply(w, th, 200);
    free(th);
    w_input(w, "hello");
    w_reply(w, OVERFLOW_REPLY, 400);
    w_reply_summary(w, "S");
    w_reply_text(w, "fits now");
    w_input(w, "big paste");
    CHECK(w_said(w, "(the conversation no longer fits the model's context window: compacting it)"));
    CHECK_INT(w->nbodies, 4);
    CHECK(strstr(w->bodies[1], "\"thinking\":\"deep\"") != 0);   // the request as it was
    CHECK(!strstr(w->bodies[2], "\"thinking\":\"deep\""));        // the summary request, trimmed
    CHECK(strstr(w->bodies[2], "big paste") && strstr(w->bodies[2], "Stop the task here"));
    CHECK_INT(count_of(w->bodies[3], "big paste"), 1);
    CHECK(w_said(w, "fits now"));
    CHECK(!w_said(w, "API error"));
    w_free(w);

    // Once: if it still does not fit after that, the message itself is too big.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = DEFAULT_COMPACT_AT));
    w_reply_text(w, "one");
    w_input(w, "hello");
    w_reply(w, OVERFLOW_REPLY, 400);
    w_reply_summary(w, "S");
    w_reply(w, OVERFLOW_REPLY, 400);
    w_input(w, "huge");
    CHECK_INT(w->nbodies, 4);
    CHECK(w_said(w, "The message does not fit the model's context window: send less at a time."));
    CHECK_INT(n_said(w, "compacting it"), 1);
    w_free(w);

    // The summary request fails too: nothing more to do but say so.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = DEFAULT_COMPACT_AT));
    w_reply_text(w, "one");
    w_input(w, "hello");
    w_reply(w, OVERFLOW_REPLY, 400);
    w_reply(w, OVERFLOW_REPLY, 400);
    w_input(w, "more");
    CHECK(w_said(w, "Could not compact the conversation: API error (HTTP 400, invalid_request_error): prompt is too long"));
    CHECK(w_said(w, "\nThe conversation no longer fits the model's context window. /clear starts over."));
    w_event(w, EV_EOF, 0, 0, 0, 0);
    CHECK(w->done && w->exit_code == 1);
    w_free(w);

    // With automatic compaction off, an overflow says what to do.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "hello");
    w_reply(w, OVERFLOW_REPLY, 400);
    w_input(w, "more");
    CHECK_INT(w->nbodies, 2);
    CHECK(w_said(w, "The conversation no longer fits the model's context window. /compact summarizes it to make room; /clear starts over."));
    w_free(w);

    // Ctrl-C during a compaction by hand: stopped, and the conversation is as it was.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "hello");
    w->hold_http = 1;
    w_input(w, "/compact");
    int64_t op = w_last_op(w, ACT_HTTP);
    CHECK(agent_busy(w->g));
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_op(w, op)->cancelled);
    CHECK(w_said(w, "Interrupted; the conversation was not compacted."));
    CHECK(!agent_busy(w->g));
    w->hold_http = 0;
    w_reply_text(w, "two");
    w_input(w, "next");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "next");
    free(t);
    CHECK(strstr(last_body(w), "hello") && strstr(last_body(w), "\"one\""));
    w_free(w);

    // Ctrl-C during one that a request waits on: the request counts as interrupted, as always.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 1000));
    w_reply_big(w, "one", 2000);
    w_input(w, "a");
    w->hold_http = 1;
    w_input(w, "b");
    CHECK(w_said(w, "(compacting"));
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_said(w, "Interrupted; the conversation was not compacted."));
    w->hold_http = 0;
    w_input(w, "/compact off");
    w_reply_text(w, "three");
    w_input(w, "c");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "b|" INTERRUPTED_NOTE "|c");
    free(t);
    w_free(w);

    // Ctrl-C while a summary request waits to be retried: the timer is stopped.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "a");
    w_reply(w, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"busy\"}}", 529);
    w_input(w, "/compact");
    int64_t timer = w_last_op(w, ACT_TIMER);
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(w_op(w, timer)->cancelled);
    CHECK(w_said(w, "Interrupted; the conversation was not compacted."));
    w_event(w, EV_TIMER, timer, 0, 0, 0);                  // it fires anyway: ignored
    CHECK_INT(w->nbodies, 2);
    w_free(w);

    // The settings.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = DEFAULT_COMPACT_AT));
    w_input(w, "/compact at");
    CHECK(w_said(w, "Compaction: automatic once the conversation takes 150000 tokens. /compact compacts it now"));
    w_input(w, "/compact at 2k");
    CHECK(w_said(w, "Compaction: automatic once the conversation takes 2000 tokens."));
    w_input(w, "/compact at\t3k");
    CHECK(w_said(w, "Compaction: automatic once the conversation takes 3000 tokens."));
    w_input(w, "/compact off");
    CHECK(w_said(w, "Automatic compaction is off. /compact still compacts when you ask; /compact at N turns it back on."));
    w_input(w, "/compact at");
    CHECK(w_said(w, "Compaction: automatic compaction is off."));
    w_input(w, "/compact at banana");
    CHECK(w_said_as(w, "Unknown size banana. /compact compacts it now", SAY_ERROR));
    CHECK(w_said(w, "The smallest is 1000."));
    w_input(w, "/compact at 500");
    CHECK(w_said(w, "Unknown size 500."));
    w_input(w, "/compact at");
    CHECK_INT(n_said(w, "automatic compaction is off."), 2);  // a bad setting changes nothing
    CHECK_INT(w->nbodies, 0);
    w_input(w, "/help");
    CHECK(w_said(w, "/compact [FOCUS]"));
    w_free(w);

    // The setting is reached, not only passed.
    w = w_new_opts(OPTS(.auto_approve = 1, .compact_at = 2020));
    w_reply_big(w, "one", 2000);
    w_reply_summary(w, "S");
    w_reply_text(w, "two");
    w_input(w, "a");
    w_input(w, "b");
    CHECK(w_said(w, "(compacting"));
    w_free(w);

    // By hand, nothing is trimmed: the summary request has the thinking too. Afterwards the
    // conversation's size is the summary's, not what it was.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply(w, "{\"type\":\"message\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"deep\",\"signature\":\"sig\"},"
               "{\"type\":\"text\",\"text\":\"one\"}],\"stop_reason\":\"end_turn\","
               "\"usage\":{\"input_tokens\":2000,\"output_tokens\":20}}", 200);
    w_input(w, "a");
    w_reply_summary(w, "S");
    w_input(w, "/compact");
    CHECK(strstr(w->bodies[1], "\"thinking\":\"deep\"") != 0);
    w_input(w, "/compact at");
    CHECK(w_said(w, "the conversation takes about ") && !w_said(w, "about 2020"));
    // /clear forgets the size with the rest.
    w_input(w, "/clear");
    int before = w->nsays;
    w_input(w, "/compact at");
    CHECK_INT(w->nsays, before + 1);
    CHECK(!strstr(w->says[before], "takes about"));
    w_free(w);

    // The server cannot be reached for the summary: the retries, then the conversation as it was.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "a");
    for (int i = 0; i < 6; i++) w_reply(w, "", 0);
    w_input(w, "/compact");
    for (int i = 0; i < 5; i++) w_event(w, EV_TIMER, w_last_op(w, ACT_TIMER), 0, 0, 0);
    CHECK_INT(w->nbodies, 7);
    CHECK(w_said(w, "Could not compact the conversation: The request failed and retrying did not help: could not connect\n"
                    "It is unchanged."));
    CHECK(!agent_busy(w->g));
    w_reply_text(w, "two");
    w_input(w, "b");
    CHECK(w_said_as(w, "two", SAY_ASSISTANT));
    CHECK(strstr(last_body(w), "\"one\"") != 0);
    w_free(w);

    // Only the text after the last block that is not text is what was just said.
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(w_resume(w, "{\"barb_session\":1,\"cwd\":\"/work\",\"messages\":["
                      "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]},"
                      "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"r\"}]},"
                      "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"older words\"},"
                      "{\"type\":\"image\",\"source\":{}},{\"type\":\"text\",\"text\":\"newest words\"}]}]}", 0));
    w_reply_summary(w, "S");
    w_input(w, "/compact");
    w_reply_text(w, "ok");
    w_input(w, "go");
    CHECK(strstr(last_body(w), "newest words") != 0);
    CHECK(!strstr(last_body(w), "older words"));
    w_free(w);

    // With verbose on, the summary is shown as it was written.
    w = w_new_opts(OPTS(.auto_approve = 1, .verbose = 1));
    w_reply_text(w, "one");
    w_input(w, "a");
    w_reply_summary(w, "What happened.");
    w_input(w, "/compact");
    CHECK(w_said_as(w, "What happened.", SAY_THINKING));
    w_free(w);
}

static void test_verbose(void) {
    world_t* w = w_new_opts(OPTS(.auto_approve = 1, .verbose = 1));
    char* r = mk_reply("end_turn", "[{\"type\":\"thinking\",\"thinking\":\"  pondering it  \",\"signature\":\"s\"},"
                                   "{\"type\":\"redacted_thinking\",\"data\":\"zz\"},"
                                   "{\"type\":\"thinking\",\"thinking\":\" \",\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"answer\"}]");
    w_reply(w, r, 200);
    w_reply(w, r, 200);
    w_reply(w, r, 200);
    w_input(w, "q1");
    CHECK(strstr(w->bodies[0], "\"thinking\":{\"type\":\"adaptive\",\"display\":\"summarized\"}") != 0);
    CHECK(w_said_as(w, "pondering it", SAY_THINKING));
    CHECK(!w_said(w, "  pondering"));                     // trimmed
    CHECK(w_said_as(w, "[redacted thinking]", SAY_THINKING));
    CHECK_INT(n_said(w, ""), 4);                          // pondering, redacted, answer, usage: the blank one is not said
    w_input(w, "/verbose");
    CHECK(w_said(w, "Verbose off: thinking is hidden."));
    w_input(w, "q2");
    CHECK(!strstr(w->bodies[1], "\"adaptive\""));
    CHECK(strstr(w->bodies[1], "\"thinking\":\"  pondering it  \"") != 0);  // still sent back as it came
    CHECK_INT(n_said(w, "pondering it"), 1);
    w_input(w, "/verbose");
    CHECK(w_said(w, "Verbose: the model's thinking is shown."));
    w_input(w, "q3");
    CHECK(strstr(w->bodies[2], "\"adaptive\"") != 0);
    CHECK_INT(n_said(w, "pondering it"), 2);
    w_input(w, "/verbose now");
    CHECK(w_said(w, "Unknown command /verbose now"));
    free(r);
    w_free(w);

    // Off: no thinking asked for and none shown.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply(w, "{\"type\":\"message\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"t\",\"signature\":\"s\"},"
               "{\"type\":\"text\",\"text\":\"a\"}],\"stop_reason\":\"end_turn\",\"usage\":{}}", 200);
    w_input(w, "q");
    CHECK(!strstr(w->bodies[0], "adaptive"));
    for (int i = 0; i < w->nsays; i++) CHECK(w->style[i] != SAY_THINKING);
    w_free(w);
}

static void test_sessions(void) {
    // Saved when the conversation changes: the user's message before the request, the reply after it.
    world_t* w = w_new_opts(OPTS(.auto_approve = 1, .session = ".barb/sessions/s.json"));
    CHECK_STR(agent_session_path(w->g), ".barb/sessions/s.json");
    CHECK(agent_session_path(w->g).p[agent_session_path(w->g).n] == 0);
    w_reply_text(w, "one");
    w_input(w, "hello");
    CHECK(w->ops[0].kind == ACT_WRITE && strstr(w->ops[0].c, "hello") && !strstr(w->ops[0].c, "\"one\""));
    CHECK(w->ops[1].kind == ACT_HTTP);
    const char* f = w_file(w, ".barb/sessions/s.json");
    CHECK(f && strstr(f, "\"barb_session\":1,\"model\":\"claude-test\",\"cwd\":\"/work\"") && strstr(f, "\"one\""));
    CHECK(!w_said(w, "Could not save"));
    // /clear: the old file keeps the old conversation, the new one goes on in -2.
    w_input(w, "/clear");
    CHECK_STR(agent_session_path(w->g), ".barb/sessions/s-2.json");
    w_input(w, "/clear");                                 // nothing said since: still -2
    CHECK_STR(agent_session_path(w->g), ".barb/sessions/s-2.json");
    w_reply_text(w, "two");
    w_input(w, "fresh");
    CHECK(strstr(w_file(w, ".barb/sessions/s.json"), "hello") && !strstr(w_file(w, ".barb/sessions/s.json"), "fresh"));
    CHECK(strstr(w_file(w, ".barb/sessions/s-2.json"), "fresh") && !strstr(w_file(w, ".barb/sessions/s-2.json"), "hello"));
    // A compaction starts another, and says where the whole of it is.
    w_reply_summary(w, "S");
    w_input(w, "/compact");
    CHECK(w_said(w, "(compacted: 2 messages became a summary; the whole conversation is still in .barb/sessions/s-2.json)"));
    CHECK_STR(agent_session_path(w->g), ".barb/sessions/s-3.json");
    CHECK(strstr(w_file(w, ".barb/sessions/s-3.json"), COMPACT_MARK) != 0);   // saved right away
    CHECK(strstr(w_file(w, ".barb/sessions/s-2.json"), "fresh") != 0);
    w_free(w);

    // A name without .json gets one for its parts; tool rounds and interrupts are saved too.
    w = w_new_opts(OPTS(.auto_approve = 1, .session = "conv"));
    w_put_file(w, "a", "x\n");
    w_reply_tools(w, TOOL("r1", "read_file", J({"path":"a"})));
    w->hold_http = 1;
    w_input(w, "go");
    w_event(w, EV_HTTP, w_last_op(w, ACT_HTTP), 200, 0, w->replies[w->irep++]);
    CHECK(strstr(w_file(w, "conv"), "\"tool_use\"") && strstr(w_file(w, "conv"), "\"tool_result\""));
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(strstr(w_file(w, "conv"), INTERRUPTED_NOTE) != 0);
    w_input(w, "/clear");
    CHECK_STR(agent_session_path(w->g), "conv-2.json");
    w_free(w);

    // An interrupt while a command runs is saved, with the results it made up.
    w = w_new_opts(OPTS(.auto_approve = 1, .session = "s.json"));
    w->hold_proc = 1;
    w_reply_tools(w, TOOL("r1", "bash", J({"command":"sleep 9"})));
    w_input(w, "go");
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(strstr(w_file(w, "s.json"), "Interrupted by the user before this finished.") != 0);
    w_free(w);

    // So is an interrupt during a compaction that a request waits on.
    w = w_new_opts(OPTS(.auto_approve = 1, .session = "s.json", .compact_at = 1000));
    w_reply_big(w, "one", 2000);
    w_input(w, "a");
    w->hold_http = 1;
    w_input(w, "b");
    CHECK(!strstr(w_file(w, "s.json"), INTERRUPTED_NOTE));
    w_event(w, EV_INTERRUPT, 0, 0, 0, 0);
    CHECK(strstr(w_file(w, "s.json"), INTERRUPTED_NOTE) != 0);
    w_free(w);

    // A save that fails is said once, not after every reply.
    w = w_new_opts(OPTS(.auto_approve = 1, .session = "s.json"));
    w->hold_fs = 1;
    w_reply_text(w, "one");
    w_reply_text(w, "two");
    w_input(w, "a");
    for (int i = 0; i < w->nops; i++) {
        if (w->ops[i].kind != ACT_WRITE) continue;
        ev_t e = { EV_FILE, w->ops[i].id, FS_DENIED, 0, str_of(0, 0), str_of(0, 0), 0 };
        w_push(w, e, 0, "permission denied");
    }
    w_run(w);
    CHECK(w_said_as(w, "Could not save the conversation to s.json: permission denied. Barb tries again after each reply.", SAY_ERROR));
    w_input(w, "b");
    for (int i = 0; i < w->nops; i++) {
        if (w->ops[i].kind != ACT_WRITE) continue;
        ev_t e = { EV_FILE, w->ops[i].id, FS_ERROR, 0, str_of(0, 0), str_of(0, 0), 0 };
        w_push(w, e, 0, 0);
    }
    w_run(w);
    CHECK_INT(n_said(w, "Could not save"), 1);
    CHECK_INT(w->nbodies, 2);                             // and the conversation goes on regardless
    w_free(w);

    // An answer to a save is not mistaken for anything else, and one that comes late is ignored.
    w = w_new_opts(OPTS(.auto_approve = 1, .session = "s.json"));
    w->hold_fs = 1;
    w_reply_text(w, "one");
    w_input(w, "a");
    int64_t save1 = w->ops[0].id;
    w_event(w, EV_FILE, save1, FS_OK, 0, 0);
    w_event(w, EV_FILE, save1, FS_ERROR, 0, 0);           // a second answer: not a save in flight any more
    CHECK(!w_said(w, "Could not save"));
    w_free(w);

    // Not saved: no writes at all.
    w = w_new_opts(OPTS(.auto_approve = 1));
    w_reply_text(w, "one");
    w_input(w, "a");
    w_input(w, "/clear");
    CHECK_INT(w_last_op(w, ACT_WRITE), 0);
    CHECK_INT(agent_session_path(w->g).n, 0);
    w_free(w);
}

#define SESSION_A "{\"barb_session\":1,\"model\":\"claude-old\",\"cwd\":\"/old/place\",\"messages\":[" \
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"earlier question\"}]}," \
    "{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"mulling\",\"signature\":\"s\"}," \
    "{\"type\":\"text\",\"text\":\"earlier answer\"},{\"type\":\"tool_use\",\"id\":\"b1\",\"name\":\"bash\",\"input\":{\"command\":\"make test\"}}]}," \
    "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"b1\",\"content\":\"ok\"}," \
    "{\"type\":\"text\",\"text\":\"[barb] Background task 1 finished\"}]}," \
    "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"b2\",\"name\":\"bash\",\"input\":{\"command\":\"ls\"}}]}]}"

static void test_resume(void) {
    // Shown again, the way it looked; the model is told it was resumed, and where it was saved.
    world_t* w = w_new_opts(OPTS(.auto_approve = 1, .session = "r.json"));
    CHECK(w_resume(w, SESSION_A, 1));
    CHECK(w_said_as(w, "earlier question", SAY_USER));
    CHECK(w_said_as(w, "earlier answer", SAY_ASSISTANT));
    CHECK(w_said_as(w, "make test", SAY_TOOL));
    CHECK(w_said_as(w, "ls", SAY_TOOL));
    CHECK(!w_said(w, "Background task 1"));               // Barb's own notes are not the user's words
    CHECK(!w_said(w, "mulling"));                         // thinking only with verbose
    CHECK(w_said_as(w, "(this conversation was saved in /old/place)", SAY_INFO));
    CHECK_INT(w->nbodies, 0);
    CHECK_INT(w->nops, 0);                                // nothing saved until something changes
    CHECK(!agent_busy(w->g));
    w_reply_text(w, "back");
    w_input(w, "next");
    CHECK_INT(w->nbodies, 1);                             // the check_body in the harness: b2 got its result
    CHECK_RESULT(w, "b2", "Not run: Barb was closed before this tool call finished.");
    char* t = w_last_user_text(w);
    CHECK_STR(str_z(t), "[barb] This conversation was saved and is being resumed now. Files may have changed since, "
                        "so read a file again before you edit it. It was saved in /old/place; the working directory "
                        "is now /work.|next");
    free(t);
    CHECK(strstr(last_body(w), "\"model\":\"claude-test\"") != 0);   // the platform picks the model
    CHECK(strstr(w_file(w, "r.json"), "earlier question") && strstr(w_file(w, "r.json"), "\"cwd\":\"/work\""));
    // The note goes once.
    w_reply_text(w, "again");
    w_input(w, "more");
    CHECK_INT(count_of(last_body(w), "being resumed now"), 1);
    w_free(w);

    // Not shown, and the same directory: nothing said at all.
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(w_resume(w, "{\"barb_session\":1,\"cwd\":\"/work\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}]}", 0));
    CHECK_INT(w->nsays, 0);
    w_reply_text(w, "a");
    w_input(w, "q2");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "q|[barb] This conversation was saved and is being resumed now. Files may have changed since, "
                        "so read a file again before you edit it.|q2");
    free(t);
    w_free(w);

    // With verbose, thinking is shown again too; a compacted start is said as such.
    w = w_new_opts(OPTS(.auto_approve = 1, .verbose = 1));
    CHECK(w_resume(w, SESSION_A, 1));
    CHECK(w_said_as(w, "mulling", SAY_THINKING));
    w_free(w);
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(w_resume(w, "{\"barb_session\":1,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\""
                      COMPACT_MARK " to fit\"},{\"type\":\"text\",\"text\":\"and then this\"}]}]}", 1));
    CHECK(w_said_as(w, "(the earlier part of this conversation was compacted into a summary)", SAY_INFO));
    CHECK(w_said_as(w, "and then this", SAY_USER));
    CHECK(!w_said(w, "to fit"));
    w_free(w);

    // A long one: the last 40 messages, and how many are not shown.
    char* big = malloc(64 * 1024);
    strcpy(big, "{\"barb_session\":1,\"messages\":[");
    for (int i = 0; i < 50; i++) {
        char m[160];
        snprintf(m, sizeof m, "%s{\"role\":\"%s\",\"content\":[{\"type\":\"text\",\"text\":\"m%d\"}]}",
                 i ? "," : "", i % 2 ? "assistant" : "user", i);
        strcat(big, m);
    }
    strcat(big, "]}");
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(w_resume(w, big, 1));
    CHECK(w_said(w, "(10 earlier messages not shown)"));
    CHECK(!w_said(w, "m9") && w_said_as(w, "m10", SAY_USER) && w_said_as(w, "m49", SAY_ASSISTANT));
    CHECK_INT(w->nsays, 41);
    w_free(w);
    // Exactly 40: no such line.
    strcpy(big, "{\"barb_session\":1,\"messages\":[");
    for (int i = 0; i < 40; i++) {
        char m[160];
        snprintf(m, sizeof m, "%s{\"role\":\"%s\",\"content\":[{\"type\":\"text\",\"text\":\"m%d\"}]}",
                 i ? "," : "", i % 2 ? "assistant" : "user", i);
        strcat(big, m);
    }
    strcat(big, "]}");
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(w_resume(w, big, 1));
    CHECK(!w_said(w, "not shown"));
    CHECK_INT(w->nsays, 40);
    w_free(w);
    free(big);

    // /clear after a resume: the note goes with the rest.
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(w_resume(w, SESSION_A, 0));
    w_input(w, "/clear");
    w_reply_text(w, "a");
    w_input(w, "new");
    CHECK(!strstr(last_body(w), "being resumed"));
    CHECK(!strstr(last_body(w), "earlier question"));
    w_free(w);

    // Not a session: refused, and nothing done.
    w = w_new_opts(OPTS(.auto_approve = 1));
    CHECK(!w_resume(w, "{\"hello\":1}", 1));
    CHECK(!w_resume(w, "{\"barb_session\":2,\"messages\":[]}", 1));
    CHECK_INT(w->nsays, 0);
    w_reply_text(w, "a");
    w_input(w, "hi");
    t = w_last_user_text(w);
    CHECK_STR(str_z(t), "hi");
    free(t);
    w_free(w);
}

static void test_random_events(void) {
    long total_requests = 0;
    h_quiet = 1;
    for (unsigned seed = 1; seed <= 400; seed++) {
        rnd_state = seed * 7919u;
        // Saved or not, compacting by itself or not, verbose or not, and sometimes a resumed one.
        static const int64_t ats[] = { 0, 1000, DEFAULT_COMPACT_AT };
        w_opts_t wo;
        wo.auto_approve = (int)rnd(2);                // one at a time: the same seed, the same run, on any compiler
        wo.max_turns = 1 + (int)rnd(8);
        wo.instructions = (int)rnd(2);
        wo.verbose = (int)rnd(2);
        wo.session = rnd(2) ? "s.json" : 0;
        wo.compact_at = ats[rnd(3)];
        world_t* w = w_new_opts(wo);
        if (rnd(3) == 0) w_resume(w, SESSION_A, (int)rnd(2));
        w_put_file(w, "f.txt", "x\nx\n");
        if (rnd(2)) w_put_file(w, "AGENTS.md", "Rules.\n@f.txt\n@gone\n");
        w->hold_http = (int)rnd(3) == 0;
        w->hold_proc = (int)rnd(3) == 0;
        w->ask_answer = (int)rnd(4) - 1;               // -1 leaves questions open
        w->nprocs = 1; w->procs[0].match = "echo"; w->procs[0].out = "hi\n";
        for (int i = 0; i < 20; i++) random_reply(w);
        for (int step = 0; step < 60 && !w->done; step++) {
            switch (rnd(11)) {
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
            case 9:
                switch (rnd(4)) {
                case 0: w_input(w, "/compact"); break;
                case 1: w_input(w, "/compact at 1k"); break;
                case 2: w_input(w, "/verbose"); break;
                default: w_input(w, "go on"); break;
                }
                break;
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

    // The same for a resumed conversation that is saved, compacted (by itself, then after an
    // overflow) and shown with its thinking. With enough memory it ends cleanly.
    clean = fatal = 0;
    for (int once = 0; once <= 1; once++)
    for (long k = 0; k < 900; k++) {
        mem_fail_once = once;
        mem_count = 0;
        mem_refused = 0;
        mem_fail_after = k;
        w_opts_t o = { 1, 0, 0, 1, ".barb/sessions/s.json", 1000 };
        world_t* w = w_new_opts(o);
        if (!w->g) { w_free(w); continue; }
        w_resume(w, SESSION_A, 1);
        char* th = mk_reply("end_turn", "[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"one\"}]");
        w_reply(w, th, 200);
        free(th);
        w_reply(w, OVERFLOW_REPLY, 400);   // b: compacted, trimmed, then sent again
        w_reply_summary(w, "S");
        w_reply_big(w, "two", 5000);
        w_reply_summary(w, "S2");          // c: compacted by itself first
        w_reply_text(w, "three");
        w_input(w, "a");
        w_input(w, "b");
        w_input(w, "c");
        w_input(w, "/clear");
        w_event(w, EV_EOF, 0, 0, 0, 0);
        if (w->done && w->exit_code == 1) fatal++;
        else clean++;
        if (mem_refused) { CHECK(w->done && w->exit_code == 1 && w_said(w, "Out of memory")); }
        else { CHECK(w->done && w->exit_code == 0 && !w_said(w, "Out of memory") && w_said(w, "three")); }
        mem_fail_after = -1;
        w_free(w);
        if (mem_live != 0) { printf("leak after failing allocation %ld: %ld live\n", k, mem_live); mem_live = 0; checks_failed++; }
        checks_run++;
    }
    mem_fail_after = -1;
    mem_fail_once = 0;
    CHECK(fatal > 0);
    CHECK(clean > 0);
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
    test_compact_parts();
    test_compaction();
    test_verbose();
    test_sessions();
    test_resume();
    test_api_errors();
    test_queued_and_turns();
    test_commands();
    test_stale_events();
    test_random_events();
    test_out_of_memory();
    return checks_report("test_agent");
}
