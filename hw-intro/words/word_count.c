/*

Copyright © 2019 University of California, Berkeley

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

word_count provides lists of words and associated count

Functional methods take the head of a list as first arg.
Mutators take a reference to a list as first arg.
*/
#include "assert.h"
#include "word_count.h"

/* Basic utilities */

char *new_string(char *str) {
  char *new_str = (char *) malloc(strlen(str) + 1);
  if (new_str == NULL) {
    return NULL;
  }
  return strcpy(new_str, str);
}

int init_words(WordCount **wclist) {
  /* Initialize word count.
     Returns 0 if no errors are encountered
     in the body of this function; 1 otherwise.
  */
  *wclist = NULL;
  return 0;
}

ssize_t len_words(WordCount *wchead) {
  /* Return -1 if any errors are
     encountered in the body of
     this function.
  */
  size_t len = 0;
  if (wchead == NULL) {
    return -1;
  }
  WordCount *wcnext = wchead;
  while (wcnext != NULL) {
    ++len;
    wcnext = wcnext->next;
  }
  return len;
}

WordCount *find_word(WordCount *wchead, char *word) {
  /* Return count for word, if it exists */
  WordCount *wc = NULL;
  WordCount *wcnext = wchead;
  while (wcnext != NULL) {
    if (!strcmp(wcnext->word, word)) {
      wc = wcnext;
      break;
    } else {
      wcnext = wcnext->next;
    }
  }
  return wc;
}

int add_word(WordCount **wclist, char *word) {
  /* If word is present in word_counts list, increment the count.
     Otherwise insert with count 1.
     Returns 0 if no errors are encountered in the body of this function; 1 otherwise.
  */
  if (wclist == NULL || word == NULL)
    return 1;

  WordCount *wchead = *wclist;
  WordCount *node = find_word(wchead, word);
  if (node != NULL) {
    node->count ++;
    return 0;
  }

  node = malloc(sizeof(WordCount));
  if (node == NULL) {
    goto bad;
  }
  node->word = malloc(strlen(word)+1);
  if (node->word == NULL) {
    free(node);
    goto bad;
  }
  node->count = 1;
  strcpy(node->word, word);
  if (wchead == NULL) {
    *wclist = node;
  } else {
    node->next = wchead;
    *wclist = node;
  }
  
  return 0;
bad:
    printf("malloc fault\n");
    return 1;
}

void fprint_words(WordCount *wchead, FILE *ofile) {
  /* print word counts to a file */
  if (wchead == NULL) {
    printf("wclist is NULL\n");
    return;
  }
  WordCount *wc;
  for (wc = wchead; wc; wc = wc->next) {
    fprintf(ofile, "%i\t%s\n", wc->count, wc->word);
  }
}
