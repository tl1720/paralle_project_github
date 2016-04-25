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
int check = 0;

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

class QueueHazard {
	private:
		Node *head;
		Node *tail;
	public:
		QueueHazard () {
			head = new Node();
			tail = head;
		}

		~QueueHazard () {
			delete head;
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

		void enqueue (int val, int thread_id) {
			Node *new_node = new Node(val);
			Node *old_tail, *old_next;
			while (true) {
				old_tail = tail;
				HeadHPList[thread_id].HP[0] = old_tail;
				if (tail != old_tail) {
					backoff();
					continue;
				}
				old_next = old_tail->next;
				if (tail != old_tail) {
					backoff();
					continue;
				}
				if (old_next != NULL) {
					__sync_bool_compare_and_swap(&tail, old_tail, old_next);
					backoff();
					continue;
				}
				if (__sync_bool_compare_and_swap(&tail->next, NULL, new_node)) {
					break;
				}
				backoff();
			}
			__sync_bool_compare_and_swap(&tail, old_tail, new_node);
			HeadHPList[thread_id].HP[0] = NULL;
		}

		Node * dequeue (int thread_id) {
			Node *old_tail, *old_head, *old_next; 
			Node *data = NULL;
			while (true) {
				old_head = head;
				HeadHPList[thread_id].HP[1] = old_head;
				if (head != old_head) {
					backoff();
					continue;
				}
				old_tail = tail;
				old_next = old_head->next;
				HeadHPList[thread_id].HP[2] = old_next;
				if (head != old_head) {
					backoff();
					continue;
				}
				if (old_next == NULL) {
					return NULL;
				}
				if (old_head == old_tail) {
					__sync_bool_compare_and_swap(&tail, old_tail, old_next);
					backoff();
					continue;
				}
				data = old_next;
				if (__sync_bool_compare_and_swap(&head, old_head, old_next)) {
					break;
				}
				backoff();
			}
			retire(old_head, thread_id);
			HeadHPList[thread_id].HP[1] = NULL;
			HeadHPList[thread_id].HP[2] = NULL;
			return data;
		}

};


/////////////////////////////////////////////////////
/* main */

void test_time () {
	double tstart = 0.0, ttaken = 0.0;
	QueueHazard q_lock_free_hazard;
	tstart = omp_get_wtime();

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		q_lock_free_hazard.enqueue(i, omp_get_thread_num());
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "enqueue time: " << ttaken << endl;
		
	usleep(1000);

	tstart = 0.0;
	ttaken = 0.0;
	tstart = omp_get_wtime();
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		q_lock_free_hazard.dequeue(omp_get_thread_num());
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "dequeue time: " << ttaken << endl;
}

void test_enqueue_correct () {
	QueueHazard q_lock_free_hazard;
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		q_lock_free_hazard.enqueue(i, omp_get_thread_num());
	}


	int count = 0;
	
	for (int i = 1;i <= N;i++) {
		Node *pop_val = q_lock_free_hazard.dequeue(omp_get_thread_num());
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
		cout << "Enqueue number: " << count << " , Sample number: " << N << endl;
		return ;
	}
	cout << "Enqueue Correct" << endl;
}

void test_dequeue_correct () {
	QueueHazard q_lock_free_hazard;
	for (int i = 1;i <= N;i++) {
		q_lock_free_hazard.enqueue(i, omp_get_thread_num());
	}

	usleep(1000);
	check = 1;

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		int thread_id = omp_get_thread_num();
		Node *data = q_lock_free_hazard.dequeue(thread_id);
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
		cout << "Dequeue number: " << count << " , Sample number: " << N << endl;
		return ;
	}
	
	cout << "Dequeue Correct" << endl;
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
			test_enqueue_correct();
			break;
		case 3:
			test_dequeue_correct();
			break;
		default:
			printf("error test method\n");
			return 0;
	}
	

	/*
	if (argc < 2 || argc > 2) {
		printf("error argument number\n");
		return 0;
	}

	thread_number = atoi(argv[1]);
	HeadHPList = new HPList[thread_number];
	retire_list = new List[thread_number];

	omp_set_num_threads(thread_number);

	cout << "Enqueue: " << endl; 
	double tstart = 0.0, ttaken = 0.0;
	double avg = 0.0;
	for (int i = 0;i < AVG_TIMES;i++) {
		QueueHazard q_lock_free_hazard;
		tstart = 0.0;
		ttaken = 0.0;
		tstart = omp_get_wtime();
		# pragma omp parallel for 
		for (int j = 1;j <= N;j++) {
			q_lock_free_hazard.enqueue(j, omp_get_thread_num());
		}
		ttaken = omp_get_wtime() - tstart;
		avg += ttaken;
		usleep(100);
	}
	cout << thread_number << " " << (avg/AVG_TIMES) << endl;

	usleep(1000);

	cout << "Dequeue: " << endl; 
	avg = 0.0;
	for (int i = 0;i < AVG_TIMES;i++) {
		QueueHazard q_lock_free_hazard;
		tstart = 0.0;
		ttaken = 0.0;
		tstart = omp_get_wtime();
		# pragma omp parallel for 
		for (int j = 1;j <= N;j++) {
			q_lock_free_hazard.dequeue(omp_get_thread_num());
		}
		ttaken = omp_get_wtime() - tstart;
		avg += ttaken;
		usleep(100);
	}
	cout << thread_number << " " << (avg/AVG_TIMES) << endl;
	*/

	return 0;
}