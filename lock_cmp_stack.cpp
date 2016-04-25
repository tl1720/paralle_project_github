#include <iostream>
#include <cstdlib>
#include <pthread.h>
#include <omp.h>
#include <unistd.h>
#include <time.h>
#include <map>
#include <vector>

using namespace std;

#define N 1000000
//#define N 10
#define MIN_DELAY 1
#define MAX_DELAY 64

int thread_number;
int lock_method;
map<int, int> correct_check;
vector<int> *correct_thread;


/////////////////////////////////////////////////////
/* structure definition */

typedef struct Node {
	int value;
	struct Node *next;
	
	Node () {
		value = 0;
		next = NULL;
	}

	Node (int val) {
		value = val;
		next = NULL;
	}
} Node;


typedef struct MCSNode {
	int flag;
	struct MCSNode *next;

	MCSNode () {
		flag = 1;
		next = NULL;
	}
} MCSNode;


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

class LockObject {
	public:
		LockObject () {}
		~LockObject () {}
		virtual void lock () {}
		virtual void unlock () {}
};

class MutexLock : public LockObject {
	public:
		pthread_mutex_t mutex;
		
		MutexLock () {
			pthread_mutex_init(&mutex, NULL);
		}

		~MutexLock () {
			pthread_mutex_destroy(&mutex);
		}

		void lock () {
			pthread_mutex_lock(&mutex);
		}

		void unlock () {
			pthread_mutex_unlock(&mutex);
		}
};


class TASLock : public LockObject {
	public:
		int taslock;
		
		TASLock () {
			taslock = 0;
		}

		~TASLock () {
		}

		void lock () {
			while (__sync_lock_test_and_set(&taslock, 1)) {}
		}

		void unlock () {
			__sync_lock_release(&taslock);
		}
};


class TASLockWiBackoff : public LockObject {
	public:
		int taslock;
		
		TASLockWiBackoff () {
			taslock = 0;
		}

		~TASLockWiBackoff () {
		}

		void lock () {
			while (__sync_lock_test_and_set(&taslock, 1)) {
				backoff();
			}
		}

		void unlock () {
			__sync_lock_release(&taslock);
		}
};


class TTASLock : public LockObject {
	public:
		int ttaslock;
		
		TTASLock () {
			ttaslock = 0;
		}

		~TTASLock () {
		}

		void lock () {
			while (__sync_lock_test_and_set(&ttaslock, 1)) {
				while (ttaslock) ;
			}
		}

		void unlock () {
			__sync_lock_release(&ttaslock);
		}
};


class TTASLockWiBackoff : public LockObject {
	public:
		int ttaslock;
		
		TTASLockWiBackoff () {
			ttaslock = 0;
		}

		~TTASLockWiBackoff () {
		}

		void lock () {
			while (__sync_lock_test_and_set(&ttaslock, 1)) {
				while (ttaslock) ;
				backoff();
			}
		}

		void unlock () {
			__sync_lock_release(&ttaslock);
		}
};


class MCSLock : public LockObject {
	public:
		int mcslock;
		MCSNode *local_node;
		MCSNode *mcs_tail;
		
		MCSLock () {
			local_node = new MCSNode[thread_number];
			mcs_tail = NULL;	
		}

		~MCSLock () {
		}

		void lock () {
			int thread_id = omp_get_thread_num();
			MCSNode *mynode = &local_node[thread_id];
			MCSNode *predecessor = NULL;
			mynode->flag = 0;
			mynode->next = NULL;
			while (true) {
				predecessor = mcs_tail;
				if (__sync_bool_compare_and_swap(&mcs_tail, predecessor, mynode)) {
					break;
				}
			}
			if (predecessor != NULL) {
				predecessor->next = mynode;
				while (!mynode->flag) {}
			}
		}

		void unlock () {
			int thread_id = omp_get_thread_num();
			MCSNode *mynode = &local_node[thread_id];
			if (mcs_tail == mynode) {
				if (__sync_bool_compare_and_swap(&mcs_tail, mynode, NULL)) {
					return ;
				}
			}
			while (mynode->next == NULL) {}
			(mynode->next)->flag = 1;
		}
};


class MCSLockWiBackoff : public LockObject {
	public:
		int mcslock;
		MCSNode *local_node;
		MCSNode *mcs_tail;
		
		MCSLockWiBackoff () {
			local_node = new MCSNode[thread_number];
			mcs_tail = NULL;	
		}

		~MCSLockWiBackoff () {
		}

		void lock () {
			int thread_id = omp_get_thread_num();
			MCSNode *mynode = &local_node[thread_id];
			MCSNode *predecessor = NULL;
			mynode->flag = 0;
			mynode->next = NULL;
			while (true) {
				predecessor = mcs_tail;
				if (__sync_bool_compare_and_swap(&mcs_tail, predecessor, mynode)) {
					break;
				}
				backoff();
			}
			if (predecessor != NULL) {
				predecessor->next = mynode;
				while (!mynode->flag) { }
			}
			
		}

		void unlock () {
			int thread_id = omp_get_thread_num();
			MCSNode *mynode = &local_node[thread_id];
			if (mcs_tail == mynode) {
				if (__sync_bool_compare_and_swap(&mcs_tail, mynode, NULL)) {
					return ;
				}
			}
			while (mynode->next == NULL) {  }
			(mynode->next)->flag = 1;
			backoff();
		}
};


class StackLockCmp {
	private:
		Node *top;
		
	public:
		LockObject *rw_lock; 

		StackLockCmp () {
			top = new Node();
			rw_lock = NULL;
		}

		~StackLockCmp () {
			delete top;
		}

		void push (int val) {
			Node *new_node = new Node(val);
			rw_lock->lock();
			new_node->next = top;
			top = new_node;
			rw_lock->unlock();
		}

		Node * pop () {
			rw_lock->lock();
			Node *pop_node = top;
			if (top->next != NULL) {
				top = top->next;
			}
			rw_lock->unlock();
			return pop_node;
		}
};


/////////////////////////////////////////////////////
/* main */

void test_time (LockObject *rw_method) {
	double tstart = 0.0, ttaken = 0.0;
	StackLockCmp s_lock_cmp;
	s_lock_cmp.rw_lock = rw_method;
	tstart = omp_get_wtime();
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_cmp.push(i);
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "push time: " << ttaken << endl;
	
	usleep(1000);

	tstart = 0.0;
	ttaken = 0.0;
	tstart = omp_get_wtime();
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_cmp.pop();
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "pop time: " << ttaken << endl;

}

void test_push_correct (LockObject *rw_method) {
	StackLockCmp s_lock_cmp;
	s_lock_cmp.rw_lock = rw_method;

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_cmp.push(i);
	}

	int count = 0;
	for (int i = 1;i <= N;i++) {
		Node *pop_val = s_lock_cmp.pop();
		if (pop_val == NULL) {
			break;
		}
		count++;
		if (correct_check[pop_val->value] == 0) {
			cout << "Unseen variable " << pop_val->value << endl;
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

void test_pop_correct (LockObject *rw_method) {
	StackLockCmp s_lock_cmp;
	s_lock_cmp.rw_lock = rw_method;
	for (int i = 1;i <= N;i++) {
		s_lock_cmp.push(i);
	}

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		Node *data = s_lock_cmp.pop();
		correct_thread[omp_get_thread_num()].push_back(data->value);
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

	if (argc < 4 || argc > 4) {
		printf("error argument number\n");
		return 0;
	}

	for (int i = 1;i <= N;i++) {
		correct_check[i] = 1;
	}

	thread_number = atoi(argv[1]);
	correct_thread = new vector<int>[thread_number];
	lock_method = atoi(argv[2]);
	int test_method = atoi(argv[3]);
	omp_set_num_threads(thread_number);

	LockObject *rw_method = NULL;

	switch (lock_method) {
		case 1:
			rw_method = new MutexLock();
			break;
		case 2:
			rw_method = new TASLock();
			break;
		case 3:
			rw_method = new TASLockWiBackoff();
			break;
		case 4:
			rw_method = new TTASLock();
			break;
		case 5:
			rw_method = new TTASLockWiBackoff();
			break;
		case 6:
			rw_method = new MCSLock();
			break;
		case 7:
			rw_method = new MCSLockWiBackoff();
			break;
		default:
			cout << "error lock method" << endl;
			return 0;
	}

	switch (test_method) {
		case 1:
			test_time(rw_method);
			break;
		case 2:
			test_push_correct(rw_method);
			break;
		case 3:
			test_pop_correct(rw_method);
			break;
		default:
			printf("error test method\n");
			return 0;
	}

	return 0;
}
