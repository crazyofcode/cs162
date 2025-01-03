/**
 * The MapReduce coordinator.
 */

#include "coordinator.h"

#ifndef SIG_PF
#define SIG_PF void (*)(int)
#endif

struct job_info {
  submit_job_request *job;
  int n_map_id;
  int cnt_map_task;
  int n_reduce_id;
  int cnt_reduce_task;
  bool map_success;
  bool reduce_success;
  enum {ready, done, failed, running} state;
};
/* Global coordinator state. */
coordinator* state;

extern void coordinator_1(struct svc_req*, SVCXPRT*);

/* Set up and run RPC server. */
int main(int argc, char** argv) {
  register SVCXPRT* transp;

  pmap_unset(COORDINATOR, COORDINATOR_V1);

  transp = svcudp_create(RPC_ANYSOCK);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create udp service.");
    exit(1);
  }
  if (!svc_register(transp, COORDINATOR, COORDINATOR_V1, coordinator_1, IPPROTO_UDP)) {
    fprintf(stderr, "%s", "unable to register (COORDINATOR, COORDINATOR_V1, udp).");
    exit(1);
  }

  transp = svctcp_create(RPC_ANYSOCK, 0, 0);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create tcp service.");
    exit(1);
  }
  if (!svc_register(transp, COORDINATOR, COORDINATOR_V1, coordinator_1, IPPROTO_TCP)) {
    fprintf(stderr, "%s", "unable to register (COORDINATOR, COORDINATOR_V1, tcp).");
    exit(1);
  }

  coordinator_init(&state);

  svc_run();
  fprintf(stderr, "%s", "svc_run returned");
  exit(1);
  /* NOTREACHED */
}

/* EXAMPLE RPC implementation. */
int* example_1_svc(int* argp, struct svc_req* rqstp) {
  static int result;

  result = *argp + 1;

  return &result;
}

static path *dup_files(u_int files_len, path *files_val) {
  path *ret = (path *)malloc(files_len * sizeof(path));
  if (ret == NULL)
    return NULL;
  for (int i = 0; i < files_len; i++) {
    ret[i] = strdup(files_val[i]);
    if (ret[i] == NULL) {
      free(ret);
      return NULL;
    }
  }
  return ret;
}

static char *copy_args(u_int args_len, char *args_val) {
  char *ret = malloc(args_len + 1);
  strncpy(ret, args_val, args_len);
  ret[args_len] = '\0';
  return ret;
}

/* SUBMIT_JOB RPC implementation. */
int* submit_job_1_svc(submit_job_request* argp, struct svc_req* rqstp) {
  static int result;

  printf("Received submit job request\n");

  /* TODO */
  struct job_info *job_info;
  submit_job_request *job;
  app tmp = get_app(argp->app);
  if (tmp.name == NULL)
    result = -1;
  else {
    result = state->job_id++;
    job_info = malloc(sizeof (struct job_info));

    job      = malloc(sizeof (submit_job_request));
    job->files.files_val = dup_files(argp->files.files_len, argp->files.files_val);
    job->files.files_len = argp->files.files_len;
    job->output_dir = strdup(argp->output_dir);
    job->app = strdup(argp->app);
    job->n_reduce = argp->n_reduce;
    job->args.args_val = copy_args(argp->args.args_len, argp->args.args_val);
    job->args.args_len = argp->args.args_len;

    job_info->job = job;
    job_info->state = ready;
    job_info->n_map_id = 0;
    job_info->cnt_map_task = 0;
    job_info->n_reduce_id = 0;
    job_info->cnt_reduce_task = 0;
    job_info->map_success = false;
    job_info->reduce_success = false;
    g_hash_table_insert(state->ht, GINT_TO_POINTER(result), job_info);
  }

  /* Do not modify the following code. */
  /* BEGIN */
  struct stat st;
  if (stat(argp->output_dir, &st) == -1) {
    mkdirp(argp->output_dir);
  }

  return &result;
  /* END */
}

/* POLL_JOB RPC implementation. */
poll_job_reply* poll_job_1_svc(int* argp, struct svc_req* rqstp) {
  static poll_job_reply result;

  printf("Received poll job request\n");

  /* TODO */

  struct job_info *job = g_hash_table_lookup(state->ht, GINT_TO_POINTER(*argp));
  if (job == NULL) {
    result.invalid_job_id = true;
  } else {
    result.invalid_job_id = false;
    result.done = job->state == done;
    result.failed = job->state == failed;
  }

  return &result;
}

/* GET_TASK RPC implementation. */
get_task_reply* get_task_1_svc(void* argp, struct svc_req* rqstp) {
  static get_task_reply result;

  printf("Received get task request\n");
  result.file = "";
  result.output_dir = "";
  result.app = "";
  result.wait = true;
  result.args.args_len = 0;

  /* TODO */
  struct job_info *job_info;
  submit_job_request *job;
  for (int i = 0; i < state->job_id; i++) {
    job_info = g_hash_table_lookup(state->ht, GINT_TO_POINTER(i));
    job = job_info->job;
    if (job_info->state == ready || job_info->state == failed) {
      result.job_id = i;
      result.output_dir = strdup(job->output_dir);
      result.app = job->app;
      result.n_map = job->files.files_len;
      result.n_reduce = job->n_reduce;
      result.args.args_len = job->args.args_len;
      result.args.args_val = job->args.args_val;
      if (!job_info->map_success) {
        result.task = job_info->n_map_id++;
        if (job_info->n_map_id == job->files.files_len)
          job_info->map_success = true;
        result.file = job->files.files_val[result.task];
        result.wait = false;
        result.reduce = false;
      } else if (job_info->cnt_map_task < job->files.files_len) {
        result.task = 0;
        result.file = "";
        result.output_dir = "";
        result.wait = true;
        result.reduce = false;
      } else if (!job_info->reduce_success) {
        result.task = job_info->n_reduce_id++;
        if (job_info->n_reduce_id == job->n_reduce)
          job_info->reduce_success = true;
        result.file = "";
        result.wait = false;
        result.reduce = true;
      } else if (job_info->cnt_reduce_task < job->n_reduce) {
        result.task = 0;
        result.file = "";
        result.output_dir = "";
        result.wait = true;
        result.reduce = true;
      }
    }
  }

  return &result;
}

/* FINISH_TASK RPC implementation. */
void* finish_task_1_svc(finish_task_request* argp, struct svc_req* rqstp) {
  static char* result;

  printf("Received finish task request\n");

  /* TODO */
  struct job_info *job_info;
  job_info = g_hash_table_lookup(state->ht, GINT_TO_POINTER(argp->job_id));
  if (argp->success) {
    if (argp->reduce)
      job_info->cnt_reduce_task++;
    else
      job_info->cnt_map_task++;
    if (job_info->cnt_reduce_task == job_info->job->n_reduce)
      job_info->state = done;
  } else {
    if (argp->reduce)
      job_info->n_reduce_id = 0;
    else
      job_info->n_map_id = 0;
    job_info->state = failed;
  }

  return (void*)&result;
}

/* Initialize coordinator state. */
void coordinator_init(coordinator** coord_ptr) {
  *coord_ptr = malloc(sizeof(coordinator));

  coordinator* coord = *coord_ptr;

  /* TODO */
  coord->ht = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
  coord->job_id = 0;
}
