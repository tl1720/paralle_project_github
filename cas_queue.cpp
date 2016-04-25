#include <iostream>
#include <cstdlib>
#include <stdint.h>
#include <stdbool.h>
#include <omp.h>
#include <unistd.h>
#include <time.h>
#include <map>
#include <vector>

using namespace std;

#define N 1000000
#define MIN_DELAY 1
#define MAX_DELAY 24
#define AVG_TIMES 20

int thread_number;
map<int, int> correct_check;
vector<int> *correct_thread;

/////////////////////////////////////////////////////
/* structure definition */

struct Node;
struct Pointer;
typedef struct Node Node;
typedef struct Pointer Pointer;

struct Pointer {
	Node *data;
	unsigned long tag;
	Pointer () {
		data = NULL;
		tag = 0;
	}

	Pointer(Node *node, unsigned int version_number) {  
		data = node; 
		tag = version_number;  
	}  

	friend bool operator==(Pointer const &l, Pointer const &r) {  
		return l.data == r.data && l.tag == r.tag;  
	}  

	friend bool operator!=(Pointer const &l, Pointer const &r) {  
		return !(l == r);  
	}  

}__attribute__((aligned(16)));


struct Node {
	int value;
	Pointer next;
	Node () {
		value = 0;
	}
};


/////////////////////////////////////////////////////
/* global inline function */

inline static bool CAS_ASM_64(volatile uint64_t target[2], uint64_t compare[2], uint64_t set[2]) {
	bool z;
	__asm__ __volatile__("movq 0(%4), %%rax;"
			     "movq 8(%4), %%rdx;"
			     "lock;" "cmpxchg16b %0; setz %1"
				: "+m" (*target),
				  "=q" (z)
				: "b"  (set[0]),
				  "c"  (set[1]),
				  "q"  (compare)
				: "memory", "cc", "%rax", "%rdx");
	return z;
}


inline static bool CAS_ASM_32(volatile uint32_t target[2], uint32_t compare[2], uint32_t set[2]) {
	bool z;
   __asm__ __volatile__(
        "lock; cmpxchg8b %1;"
        "setz %0;"
            : "=r"(z), "=m"(*target)
            : "a"(compare[0]), "d" (compare[1]), "b" (set[0]), "c" (set[1])
            : "memory");
	return z;
}



inline static bool CAS2(void *t, void *c, void *s) {
	#ifdef __x86_64
		return CAS_ASM_64((uint64_t *)t, (uint64_t *)c, (uint64_t *)s);
	#else
		return CAS_ASM_32((uint32_t *)t, (uint32_t *)c, (uint32_t *)s);
	#endif
}

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

class QueueWithTag {
	private:
		Pointer head;
		Pointer tail;
	public:

		QueueWithTag () {
			Node *vnode = new Node();
			vnode->next = Pointer(NULL, 0);
			head = Pointer(vnode, 0);
			tail = Pointer(vnode, 0);
		}

		void enqueue (int val) {
			Pointer old_tail, old_next;  
			Node *data = new Node();  
			data->value = val;  
			while(true){  
				old_tail = tail;   
				old_next = old_tail.data->next;  
				if (old_tail == tail) {  
					if(old_next.data == NULL) {  
						Pointer new_pt(data, old_next.tag+1);  
						if(CAS2(&(tail.data->next), &old_next, &new_pt)){  
							Pointer new_pt(data, old_tail.tag+1); 
							tail = new_pt;
							break;
						}  
					} else {  
						Pointer new_pt(old_next.data, old_tail.tag+1);  
						CAS2(&tail, &old_tail, &new_pt);   
					}
				}  
				backoff();
			}  
			//Pointer new_pt(data, old_tail.tag+1);  
			//CAS2(&tail, &old_tail, &new_pt); 
		}

		
		Node * dequeue() {    
			Pointer old_tail, old_head, old_next;  
			Node *data = NULL;

			while(true){   
				old_head = head;   
				old_tail = tail;   
				old_next = (old_head.data)->next;   
				if (old_head != head) {
					backoff();
					continue;
				}   
  
				if(old_head.data == old_tail.data){  
					if (old_next.data == NULL){   
						return NULL;  
					}  
					Pointer new_pt(old_next.data, old_tail.tag+1);  
					CAS2(&tail, &old_tail, &new_pt);  
				} else{   
					data = old_next.data;  
					Pointer new_pt(old_next.data, old_head.tag+1);  
					if(CAS2(&head, &old_head, &new_pt)){  
						break;  
					}  
				}  
				backoff();
			}  
			//delete old_head.data;  
			return data;  
		} 
		
};

/////////////////////////////////////////////////////
/* main */

void test_time () {
	double tstart = 0.0, ttaken = 0.0;
	QueueWithTag q_lock_free_tag;
	tstart = omp_get_wtime();

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		q_lock_free_tag.enqueue(i);
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "enqueue time: " << ttaken << endl;
		
	usleep(1000);

	tstart = 0.0;
	ttaken = 0.0;
	tstart = omp_get_wtime();
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		q_lock_free_tag.dequeue();
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "dequeue time: " << ttaken << endl;
}

void test_enqueue_correct () {
	QueueWithTag q_lock_free_tag;
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		q_lock_free_tag.enqueue(i);
	}

	int count = 0;
	for (int i = 1;i <= N;i++) {
		Node *pop_val = q_lock_free_tag.dequeue();
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
	QueueWithTag q_lock_free_tag;
	for (int i = 1;i <= N;i++) {
		q_lock_free_tag.enqueue(i);
	}

	usleep(1000);

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		Node *data = q_lock_free_tag.dequeue();
		correct_thread[omp_get_thread_num()].push_back(data->value);
	}
	
	int count = 0;
	for (int i = 0;i < thread_number;i++) {
		for (int j = 0;j < correct_thread[i].size();j++) {
			count++;
			int pop_val = correct_thread[i][j];
			if (correct_check[pop_val] == 0) {
				cout << "Unseen variable " << pop_val << endl;
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


int main(int argc, char *argv[]) {
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
	omp_set_num_threads(thread_number);

	cout << "Enqueue: " << endl; 

	double tstart = 0.0, ttaken = 0.0;
	double avg = 0.0;
	for (int i = 0;i < AVG_TIMES;i++) {
		QueueWithTag q_lock_free_tag;
		tstart = 0.0;
		ttaken = 0.0;
		tstart = omp_get_wtime();
		# pragma omp parallel for 
		for (int j = 1;j <= N;j++) {
			q_lock_free_tag.enqueue(j);
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
		QueueWithTag q_lock_free_tag;
		tstart = 0.0;
		ttaken = 0.0;
		tstart = omp_get_wtime();
		# pragma omp parallel for 
		for (int j = 1;j <= N;j++) {
			q_lock_free_tag.dequeue();
		}
		ttaken = omp_get_wtime() - tstart;
		avg += ttaken;
		usleep(100);
	}

	cout << thread_number << " " << (avg/AVG_TIMES) << endl;
	*/
	

	return 0;
}