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

class StackWithTag {
	private:
		Pointer top;
	public:

		StackWithTag () {
			Node *vnode = new Node();
			vnode->next = Pointer(NULL, 0);
			top = Pointer(vnode, 0);
		}

		void push (int val) {
			Pointer old_top;  
			Node *data = new Node();  
			data->value = val; 
			while(true){  
				old_top = top;
				Pointer new_pt(top.data, old_top.data->next.tag+1); 
				data->next = new_pt;
				Pointer new_top(data, old_top.tag+1); 
				if (CAS2(&top, &old_top, &new_top)) {
					break;
				}
				backoff();
			}  
		}

		Node * pop () {
			Pointer old_top, old_next;  
			Node *data = NULL;
			while (true) {
				old_top = top;
				old_next = (old_top.data)->next;
				if (old_next.data == NULL) {
					return NULL;
				}
				if (CAS2(&top, &old_top, &old_next)) {
					data = old_top.data;
					break;
				}
				backoff();
			}
			return data;
		}		
};

/////////////////////////////////////////////////////
/* main */

void test_time () {
	double tstart = 0.0, ttaken = 0.0;
	StackWithTag s_lock_free_tag;
	tstart = omp_get_wtime();

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_free_tag.push(i);
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "push time: " << ttaken << endl;
		
	usleep(1000);

	tstart = 0.0;
	ttaken = 0.0;
	tstart = omp_get_wtime();
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_free_tag.pop();
	}
	ttaken = omp_get_wtime() - tstart;
	cout << "pop time: " << ttaken << endl;
}

void test_push_correct () {
	StackWithTag s_lock_free_tag;
	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		s_lock_free_tag.push(i);
	}


	int count = 0;
	for (int i = 1;i <= N;i++) {
		Node *pop_val = s_lock_free_tag.pop();
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
	StackWithTag s_lock_free_tag;
	for (int i = 1;i <= N;i++) {
		s_lock_free_tag.push(i);
	}

	# pragma omp parallel for 
	for (int i = 1;i <= N;i++) {
		Node *data = s_lock_free_tag.pop();
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