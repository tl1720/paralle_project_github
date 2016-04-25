#include <iostream>
#include <cstdlib>
#include <omp.h>
#include <unistd.h>
#include <time.h>
#include <map>
#include <vector>

using namespace std;

#define K 4
#define R 8
#define N 1000000
#define MIN_DELAY 1
#define MAX_DELAY 16
class List;

/////////////////////////////////////////////////////
/* structure definition */

typedef struct Node {
	int value;
	Node *next;

	Node () {
		value = 0;
		next = NULL;
	}
	
	Node (int val) {
		value = val;
		next = NULL;
	}
	
} Node;

typedef struct HPList {
	Node *HP[K];
	HPList () {
		for (int i = 0;i < K;i++) {
			HP[i] = NULL;
		}
	}
} HPList;

typedef struct ListElement {
	Node *data;
	struct ListElement *next;

	ListElement () {
		data = NULL;
		next = NULL;
	}

	ListElement (Node *node) {
		data = node;
		next = NULL;
	}

} ListElement;

/////////////////////////////////////////////////////
/* global variable */

HPList *HeadHPList;
List *retire_list;
int thread_number;
map<int, int> correct_check;
vector<int> *correct_thread;

/////////////////////////////////////////////////////
/* global function */

void backoff () {
	static int limit = MIN_DELAY;
	int delay = rand()%limit*100;
	limit = min(MAX_DELAY, 2*limit);
	usleep(delay);
}

/////////////////////////////////////////////////////
/* class definition */

class List {
	private:
		ListElement *head;
		ListElement *tail;
	public:
		int size;
		List () {
			head = new ListElement();
			tail = head;
		}
		
		~List () {
			delete head;
		}
		
		void insert (Node *node) {
			ListElement *new_element = new ListElement(node);
			tail->next = new_element;
			tail = new_element;
			size++;
		}

		void erase (ListElement *key) {
			ListElement *cur = head->next;
			ListElement *pre = head;
			while (cur != NULL) {
				if (key->data == cur->data) {
					pre->next = cur->next;
					//delete cur->data;
					size--;
					break;
				}
				pre = cur;
				cur = cur->next;
			}
		}

		void clearall() {
			delete head;
			head = new ListElement();
			tail = head;
		}

		ListElement * gethead() {
			return head->next;
		}

		bool find (ListElement **key) {
			if (*key == NULL) { return false; }
			if (head == NULL) { return false; }
			ListElement *cur = head->next;
			while (cur != NULL) {
				if (*key == NULL || cur == NULL) { return false; }
				if ((*key)->data == cur->data) {
					return true;
				}
				cur = cur->next;
			}
			return false;
		}

		void show () {
			ListElement *cur = head->next;
			cout << "list: "; 
			while (cur != NULL) {
				cout << cur->data->value << " ";
				cur = cur->next;
			}
			cout << endl;
		}
};

class StackHazard {
	private:
		Node *top;
	public:
		StackHazard () {
			top = new Node();
		}

		~StackHazard () {
			delete top;
		}

		void retire (Node *node, int thread_id) {
			retire_list[thread_id].insert(node);
			if (retire_list[thread_id].size >= R) {
				scan(thread_id);
			}
		}

		void scan (int thread_id) {
			List *private_list = new List();
			for (int i = 0;i < thread_number;i++) {
				for (int j = 0;j < K;j++) {
					Node *hptr = HeadHPList[i].HP[j];
					if (hptr != NULL) {
						private_list->insert(hptr);
					}
				}
			}

			ListElement *cur = retire_list[thread_id].gethead();
			while (cur != NULL) {
				if (private_list == NULL) {return ;}
				if (!(private_list->find(&cur))) {
					retire_list[thread_id].erase(cur);
				}
				cur = cur->next;
			}

			delete private_list;
		}

		void push (int val, int thread_id) {
			Node *new_node = new Node(val);
			Node *old_top;
			while (true) {
				old_top = top;
				HeadHPList[thread_id].HP[0] = old_top;
				if (top != old_top) {
					backoff();
					continue;
				}
				new_node->next = old_top;
				if (__sync_bool_compare_and_swap(&top, old_top, new_node)) {
					break;
				}
				backoff();
			}
			HeadHPList[thread_id].HP[0] = NULL;
		}

		
		Node * pop (int thread_id) {
			Node *old_top, *old_next;
			Node *data = NULL;
			while (true) {
				old_top = top;
				HeadHPList[thread_id].HP[1] = old_top;
				if (top != old_top) {
					backoff();
					continue;
				}

				old_next = old_top->next;

				HeadHPList[thread_id].HP[2] = old_next;
				if (top != old_top) {
					backoff();
					continue;
				}

				if (old_next == NULL) {
					return NULL;
				}

				if (__sync_bool_compare_and_swap(&top, old_top, old_next)) {
					data = old_top;
					break;
				} 
				backoff();
			}
			//cout << "old_top: " << old_top << endl;
			retire(old_top, thread_id);
			HeadHPList[thread_id].HP[1] = NULL;
			HeadHPList[thread_id].HP[2] = NULL;
			return data;
		}
		
};


/////////////////////////////////////////////////////
/* main */

void test_time () {
	double tstart = 0.0, ttaken = 0.0;
	StackHazard s_lock_free_hazard;
	tstart = omp_get_wtime();

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_free_hazard.push(i, omp_get_thread_num());
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "push time: " << ttaken << endl;
		
	usleep(1000);

	tstart = 0.0;
	ttaken = 0.0;
	tstart = omp_get_wtime();
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_free_hazard.pop(omp_get_thread_num());
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "pop time: " << ttaken << endl;
}

void test_push_correct () {
	StackHazard s_lock_free_hazard;
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_free_hazard.push(i, omp_get_thread_num());
	}


	int count = 0;
	
	for (int i = 1;i <= N;i++) {
		Node *pop_val = s_lock_free_hazard.pop(omp_get_thread_num());
		if (pop_val == NULL) {
			break;
		}
		count++;
		
		if (correct_check[pop_val->value] == 0) {
			cout << "Unseen variable" << endl;
			return ;
		}
		
		correct_check[pop_val->value]--;
		if (correct_check[pop_val->value] < 0) {
			cout << "Multiple variable" << endl;
			return ;
		}
		
	}
	
	if (count != N) {
		cout << "Push number: " << count << " , Sample number: " << N << endl;
		return ;
	}
	cout << "Push Correct" << endl;
}

void test_pop_correct () {
	StackHazard s_lock_free_hazard;
	for (int i = 1;i <= N;i++) {
		s_lock_free_hazard.push(i, omp_get_thread_num());
	}

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		int thread_id = omp_get_thread_num();
		Node *data = s_lock_free_hazard.pop(thread_id);
		correct_thread[thread_id].push_back(data->value);
	}

	int count = 0;
	for (int i = 0;i < thread_number;i++) {
		for (int j = 0;j < correct_thread[i].size();j++) {
			count++;
			int pop_val = correct_thread[i][j];
			if (correct_check[pop_val] == 0) {
				cout << "Unseen variable" << endl;
				return ;
			}
			correct_check[pop_val]--;
			if (correct_check[pop_val] < 0) {
				cout << "Multiple variable" << endl;
				return ;
			}
		}
	}

	if (count != N) {
		cout << "Push number: " << count << " , sample number: " << N << endl;
		return ;
	}
	cout << "Pop Correct" << endl;
}

int main (int argc, char *argv[]) {
	if (argc < 3 || argc > 3) {
		printf("error argument number\n");
		return 0;
	}

	for (int i = 1;i <= N;i++) {
		correct_check[i] = 1;
	}
	
	thread_number = atoi(argv[1]);
	correct_thread = new vector<int>[thread_number];
	int test_method = atoi(argv[2]);

	HeadHPList = new HPList[thread_number];
	retire_list = new List[thread_number];

	omp_set_num_threads(thread_number);

	switch (test_method) {
		case 1:
			test_time();
			break;
		case 2:
			test_push_correct();
			break;
		case 3:
			test_pop_correct();
			break;
		default:
			printf("error test method\n");
			return 0;
	}

	return 0;
}