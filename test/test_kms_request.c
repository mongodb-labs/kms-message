/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/kms_message/kms_message.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <src/hexlify.h>
#include <src/kms_request_str.h>

const char *aws_test_suite_dir = "aws-sig-v4-test-suite";

const char *skipped_aws_tests[] = {
   /* we don't yet support temporary security credentials provided by the AWS
    * Security Token Service (AWS STS). see post-sts-token/readme.txt */
   "post-sts-token",
};

bool
skip_aws_test (const char *test_name)
{
   size_t i;

   for (i = 0; i < sizeof (skipped_aws_tests) / sizeof (char *); i++) {
      if (0 == strcmp (test_name, skipped_aws_tests[i])) {
         return true;
      }
   }

   return false;
}

bool
ends_with (const char *str, const char *suffix)
{
   size_t str_len = strlen (str);
   size_t suf_len = strlen (suffix);
   if (str_len >= suf_len &&
       0 == strncmp (&str[str_len - suf_len], suffix, suf_len)) {
      return true;
   }

   return false;
}


const char *
last_segment (const char *str)
{
   const char *p = str + strlen (str);

   while (--p > str) {
      if (*p == '/') {
         return strdup (p + 1);
      }
   }

   return strdup (str);
}

char *
aws_test_file_path (const char *path, const char *suffix)
{
   char *r;
   const char *test_name = last_segment (path);
   char file_path[PATH_MAX];
   snprintf (file_path, PATH_MAX, "%s/%s.%s", path, test_name, suffix);
   r = strdup (file_path);
   return r;
}

char *
read_aws_test (const char *path, const char *suffix)
{
   char *file_path = aws_test_file_path (path, suffix);
   FILE *f;
   struct stat file_stat;
   size_t f_size;
   char *str;

   if (0 != stat (file_path, &file_stat)) {
      perror (file_path);
      abort ();
   }

   f = fopen (file_path, "r");
   if (!f) {
      perror (file_path);
      abort ();
   }

   f_size = (size_t) file_stat.st_size;
   str = malloc (f_size + 1);
   if (f_size != fread (str, 1, f_size, f)) {
      perror (file_path);
      abort ();
   }

   fclose (f);
   free (file_path);

   str[f_size] = '\0';

   return str;
}

void
set_test_date (kms_request_t *request)
{
   struct tm tm;

   /* all tests use the same date and time */
   assert (strptime ("20150830T123600Z", "%Y%m%dT%H%M%SZ", &tm));
   assert (kms_request_set_date (request, &tm));
}

kms_request_t *
read_req (const char *path)
{
   kms_request_t *request;
   char *file_path = aws_test_file_path (path, "req");
   FILE *f;
   size_t len;
   ssize_t line_len;
   char *line = NULL;
   char *method;
   char *uri_path;
   char *field_name;
   char *field_value;
   bool r;

   f = fopen (file_path, "r");
   if (!f) {
      perror (file_path);
      abort ();
   }

   /* like "GET /path HTTP/1.1" */
   line_len = getline (&line, &len, f);
   method = strndup (line, strchr (line, ' ') - line);
   uri_path = strndup (line + strlen (method) + 1,
                       line_len - strlen (method) - 1 - strlen (" HTTP/1.1\n"));

   request = kms_request_new (method, uri_path);
   /* from docs.aws.amazon.com/general/latest/gr/signature-v4-test-suite.html */
   kms_request_set_region (request, "us-east-1");
   kms_request_set_service (request, "service");
   kms_request_set_access_key_id (request, "AKIDEXAMPLE");
   kms_request_set_secret_key (request,
                               "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");

   while ((line_len = getline (&line, &len, f)) != -1) {
      if (strchr (line, ':')) {
         /* new header field like Host:example.com */
         field_name = strtok (line, ": ");
         assert (field_name);
         field_value = strtok (NULL, "\n");
         assert (field_value);
         r = kms_request_add_header_field (request, field_name, field_value);
         assert (r);
      } else if (0 == strcmp (line, "\n")) {
         /* end of header */
         break;
      } else if (line_len > 2) {
         /* continuing a multiline header from previous line */
         /* TODO: is this a test quirk or HTTP specified behavior? */
         kms_request_append_header_field_value (request, "\n", 1);
         /* omit this line's newline */
         kms_request_append_header_field_value (
            request, line, (size_t) (line_len - 1));
      }
   }

   while ((line_len = getline (&line, &len, f)) != -1) {
      kms_request_append_payload (request, line, (size_t) line_len);
   }

   fclose (f);
   free (file_path);

   set_test_date (request);

   return request;
}

static ssize_t
first_non_matching (const char *x, const char *y)
{
   size_t len = strlen (x) > strlen (y) ? strlen (x) : strlen (y);
   size_t i;

   for (i = 0; i < len; i++) {
      if (x[i] != y[i]) {
         return i;
      }
   }

   if (strlen (x) > strlen (y)) {
      return strlen (y) + 1;
   }

   if (strlen (y) > strlen (x)) {
      return strlen (x) + 1;
   }

   /* the strings match */
   return -1;
}

void
aws_sig_v4_test_compare (kms_request_t *request,
                         char *(*func) (kms_request_t *),
                         const char *dir_path,
                         const char *suffix)
{
   const char *test_name = last_segment (dir_path);
   char *expect;
   char *actual;

   /* canonical request */
   expect = read_aws_test (dir_path, suffix);
   actual = func (request);

   if (strlen (expect) != strlen (actual) ||
       0 != memcmp (expect, actual, strlen (actual))) {
      fprintf (stderr,
               "%s.%s failed, mismatch starting at %zd\n"
               "--- Expect (%zu chars) ---\n%s\n"
               "--- Actual (%zu chars) ---\n%s\n",
               test_name,
               suffix,
               first_non_matching (expect, actual),
               strlen (expect),
               expect,
               strlen (actual),
               actual);
      abort ();
   }

   free (actual);
   free (expect);
}

void
aws_sig_v4_test (const char *dir_path)
{
   kms_request_t *request;

   request = read_req (dir_path);
   aws_sig_v4_test_compare (
      request, kms_request_get_canonical, dir_path, "creq");
   aws_sig_v4_test_compare (
      request, kms_request_get_string_to_sign, dir_path, "sts");
   aws_sig_v4_test_compare (
      request, kms_request_get_signature, dir_path, "authz");
   aws_sig_v4_test_compare (request, kms_request_get_signed, dir_path, "sreq");
   kms_request_destroy (request);
}

bool
spec_tests (const char *path, const char *selected)
{
   /* Amazon supplies tests, one per directory, 5 files per test, see
    * docs.aws.amazon.com/general/latest/gr/signature-v4-test-suite.html */
   DIR *dp;
   struct dirent *ent;
   bool ran_tests = false;
   const char *test_name = last_segment (path);
   char sub[PATH_MAX];

   dp = opendir (path);
   if (!dp) {
      perror (path);
      abort ();
   }

   if (skip_aws_test (test_name) && !selected) {
      printf ("SKIP: %s\n", test_name);
      goto done;
   }

   while ((ent = readdir (dp))) {
      if (ent->d_name[0] == '.') {
         continue;
      }

      if (ent->d_type & DT_DIR) {
         snprintf (sub, PATH_MAX, "%s/%s", path, ent->d_name);
         ran_tests |= spec_tests (sub, selected);
      }

      if (!(ent->d_type & DT_REG) || !ends_with (ent->d_name, ".req")) {
         continue;
      }

      /* "ent" is a "test.req" request file, this is a test directory */
      /* skip the test if it doesn't match the name passed to us */
      if (selected && 0 != strcmp (test_name, selected)) {
         continue;
      }

      printf ("%s\n", path);
      aws_sig_v4_test (path);
      ran_tests = true;
   }

done:
   (void) closedir (dp);

   return ran_tests;
}

/* docs.aws.amazon.com/general/latest/gr/sigv4-calculate-signature.html */
void
example_signature_test (void)
{
   const char *expect =
      "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9";
   kms_request_t *request;
   unsigned char signing[32];
   char *sig;

   request = kms_request_new ("GET", "uri");
   set_test_date (request);
   kms_request_set_region (request, "us-east-1");
   kms_request_set_service (request, "iam");
   kms_request_set_secret_key (request,
                               "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");

   assert (kms_request_get_signing_key (request, signing));
   sig = hexlify (signing, 32);
   if (strlen (expect) != strlen (sig) ||
       0 != memcmp (expect, sig, strlen (sig))) {
      fprintf (stderr,
               "%s failed\n"
               "--- Expect ---\n%s\n"
               "--- Actual ---\n%s\n",
               __FUNCTION__,
               expect,
               sig);
      abort ();
   }

   free (sig);
   kms_request_destroy (request);
}

void
path_normalization_test (void)
{
   const char *tests[][2] = {
      {"", "/"},
      {"/", "/"},
      {"/..", "/"},
      {"./..", "/"},
      {"../..", "/"},
      {"/../..", "/"},
      {"a", "a"},
      {"a/", "a/"},
      {"a//", "a/"},
      {"a///", "a/"},
      {"/a", "/a"},
      {"//a", "/a"},
      {"///a", "/a"},
      {"/a/", "/a/"},
      {"/a/..", "/"},
      {"/a/../..", "/"},
      {"/a/b/../..", "/"},
      {"/a/b/c/../..", "/a"},
      {"/a/b/../../d", "/d"},
      {"/a/b/c/../../d", "/a/d"},
      {"/a/b", "/a/b"},
      {"a/..", "/"},
      {"a/../..", "/"},
      {"a/b/../..", "/"},
      {"a/b/c/../..", "a"},
      {"a/b/../../d", "d"},
      {"a/b/c/../../d", "a/d"},
      {"a/b", "a/b"},
      {"/a//b", "/a/b"},
      {"/a///b", "/a/b"},
      {"/a////b", "/a/b"},
      {"//", "/"},
      {"//a///", "/a/"},
   };

   const char **test;
   const char *out;
   size_t i;
   kms_request_str_t *in, *norm;

   for (i = 0; i < sizeof (tests) / (2 * sizeof (const char *)); i++) {
      test = tests[i];
      in = kms_request_str_new_from_chars (test[0], -1);
      out = test[1];
      norm = kms_request_str_path_normalized (in);
      if (0 != strcmp (out, norm->str)) {
         fprintf (stderr,
                  "Path normalization test failed:\n"
                  "Input:  %s\n"
                  "Expect: %s\n"
                  "Actual: %s\n",
                  in->str,
                  out,
                  norm->str);
         abort ();
      }

      kms_request_str_destroy (in);
      kms_request_str_destroy (norm);
   }
}


#define RUN_TEST(_func)                                      \
   do {                                                      \
      if (!selector || 0 == strcasecmp (#_func, selector)) { \
         printf ("%s\n", #_func);                            \
         _func ();                                           \
         ran_tests = true;                                   \
      }                                                      \
   } while (0)


/* TODO: test multibyte UTF-8 */
int
main (int argc, char *argv[])
{
   const char *help;
   char *dir_path = NULL;
   char *selector = NULL;
   bool ran_tests = false;

   help = "Usage: test_kms_request [TEST_NAME]";

   if (argc > 2) {
      fprintf (stderr, "%s\n", help);
      abort ();
   } else if (argc == 2) {
      selector = argv[1];
   }

   RUN_TEST (example_signature_test);
   RUN_TEST (path_normalization_test);

   ran_tests |= spec_tests (aws_test_suite_dir, selector);

   if (!ran_tests) {
      assert (argc == 2);
      fprintf (stderr, "No such test: \"%s\"\n", argv[1]);
      abort ();
   }

   free (selector);
}
