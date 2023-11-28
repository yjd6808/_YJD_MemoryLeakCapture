#include <iostream>
#include <thread>
#include <map>
#include <unordered_map>

#include <Windows.h>
#include <crtdbg.h>

// 파일경로는 너무 길기때문에 파일명단위로 한다.
#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

// 라인넘버의 상위 2비트는 무슨 함수를 사용해서 동적할당했는지 기록한다.
#define OP_DBG_IGNORE			0
#define OP_DBG_NEW				1
#define OP_DBG_OPERATOR_NEW		2
#define OP_DBG_MALLOC			3


// 디버깅용 동적할당 함수를 정의한다.
#define dbg_new new (_NORMAL_BLOCK, __FILENAME__, (OP_DBG_NEW << 24) | __LINE__)
#define dbg_operator_new(sz) ::operator new (sz, _NORMAL_BLOCK, __FILENAME__, (OP_DBG_OPERATOR_NEW << 24) | __LINE__)
#define dbg_malloc(sz)  _malloc_dbg(sz, _NORMAL_BLOCK, __FILENAME__, (OP_DBG_MALLOC << 24) | __LINE__);

using line_number_t = int;
using req_number_t = long;
using file_name_t = const unsigned char*;
using dbg_op_t = char;

// 자체적으로 관리할 정보 (내가 정의한거)
struct heap_block_info
{
	struct line_number_with
	{
		line_number_with() : ops(1) {}	// 한 라인에서 동적할당은 보통 1번 일어나므로.
		std::unordered_map<dbg_op_t, int> ops;
	};

	struct name_with
	{
		name_with() : lines(5) {}
		std::unordered_map<line_number_t, line_number_with> lines;
	};

	heap_block_info(size_t b_size) 
		: block_size(b_size)
		, files(5)	// 많아봐야 2~3개 아닐까?
	{}

	const size_t block_size;
	std::unordered_map<file_name_t, name_with> files;
};

// CRT 라이브러리 소스파일에서 가져옴
struct heap_block_header 
{
	static size_t const no_mans_land_size = 4;

	heap_block_header*	block_header_next;
	heap_block_header*	block_header_prev;
	file_name_t			file_name;
	int					line_number;
	int					block_use;
	size_t				data_size;
	long				request_number;
	unsigned char       gap[no_mans_land_size];
};


// 글로벌 변수
int g_main_thread_id = GetCurrentThreadId();
std::map<size_t, heap_block_info*> g_heap_block_map;
std::map<req_number_t, heap_block_info*> g_heap_req_number_map;


void block_allocated_op(heap_block_info::line_number_with& line_number_with, dbg_op_t op) {
	auto op_t = line_number_with.ops.find(op);
	if (op_t == line_number_with.ops.end()) {
		auto ret = line_number_with.ops.insert(std::make_pair(op, 1));
	} else {
		++line_number_with.ops[op];
	}
}


void block_allocated_line(heap_block_info::name_with& name_with, line_number_t line_number, dbg_op_t op) {
	auto line_it = name_with.lines.find(line_number);
	if (line_it == name_with.lines.end()) {
		auto ret = name_with.lines.insert(std::make_pair(line_number, heap_block_info::line_number_with{}));
		block_allocated_op(ret.first->second, op);
	} else {
		block_allocated_op(name_with.lines[line_number], op);
	}
}

void block_allocated(size_t			block_size, 
					 file_name_t	file_name, 
					 line_number_t	line_number,
					 req_number_t   request_number,
					 dbg_op_t		op)
{
	heap_block_info* block_info = nullptr;
	auto block_it = g_heap_block_map.find(block_size);
	if (block_it == g_heap_block_map.end()) {
		block_info = new heap_block_info(block_size);
		g_heap_block_map.insert(std::make_pair(block_size, block_info));
	} else {
		block_info = block_it->second;
	}

	auto file_it =  block_info->files.find(file_name);
	if (file_it == block_info->files.end()) {
		auto ret = block_info->files.insert(std::make_pair(file_name, heap_block_info::name_with{}));
		block_allocated_line(ret.first->second, line_number, op);
	} else {
		block_allocated_line(block_info->files[file_name], line_number, op);
	}

	g_heap_req_number_map.insert(std::make_pair(request_number, block_info));
}

void block_deallocated(heap_block_header* block_header) {

	
	dbg_op_t op = (block_header->line_number & 0xff000000) >> 24;

	if (op == OP_DBG_IGNORE) {
		return;
	}

	auto it = g_heap_req_number_map.find(block_header->request_number);
	if (it == g_heap_req_number_map.end()) {
		fprintf(stderr, "%d 요청번호 못찾음\n", block_header->request_number);
		return;
	}

	heap_block_info* block_info = it->second;
	line_number_t line_number = (block_header->line_number & 0x00ffffff);
	auto& lines = block_info->files[block_header->file_name].lines[line_number];
	int& count = lines.ops[op];
	--count;

	if (count == 0) {
		lines.ops.erase(op);
	}

	g_heap_req_number_map.erase(block_header->request_number);
}

void block_diff(bool clear = true) {

	constexpr auto fn_op_name = [](dbg_op_t op)->const char* {
		if (op == OP_DBG_NEW)				return "new";
		else if (op == OP_DBG_OPERATOR_NEW) return "operator new";
		else if (op == OP_DBG_MALLOC)		return "malloc";
		return "unknown";
	};


	for (auto block_it = g_heap_block_map.begin(); block_it != g_heap_block_map.end(); ++block_it) {
		size_t block_size = block_it->first;
		auto& files = block_it->second->files;
		for (auto file_it = files.begin(); file_it != files.end(); ++file_it) {
			file_name_t file_name = file_it->first;
			auto& lines = file_it->second.lines;
			for (auto line_it = lines.begin(); line_it != lines.end(); ++line_it) {
				line_number_t line_number = line_it->first;
				auto& ops = line_it->second.ops;
				for (auto op_it = ops.begin(); op_it != ops.end(); ++op_it) {
					dbg_op_t op = op_it->first;
					int count = op_it->second;
					printf(
						"SIZE: %6d | FILE: %25s:%05d | OP: %12s | COUNT: %6d\n"
						, block_size
						, file_name
						, line_number
						, fn_op_name(op)
						, count
					);
				}
			}
		}
	}

	if (clear) {
		for (auto block_it = g_heap_block_map.begin(); block_it != g_heap_block_map.end(); ++block_it) {
			delete block_it->second;
		}

		g_heap_block_map.clear();
		g_heap_req_number_map.clear();
	}
	
}



int IGotYou(int           alloc_type,
            void*         user_data,
            size_t        size,
            int           block_type,
			req_number_t  request_number,
            file_name_t	  file_name,
            line_number_t line_number)
{
	if (block_type != _NORMAL_BLOCK)
		return TRUE;

	// 다른 쓰레드에서 생성된 메모리도 캡쳐하고 싶으면 주석 해제
	// if (GetCurrentThreadId() != g_main_thread_id) {
	// 	return TRUE;
	// }

	if (alloc_type == _HOOK_ALLOC) {
		line_number_t extracted_line_number = (line_number & 0x00ffffff);
		dbg_op_t op = (line_number & 0xff000000) >> 24;

		// 일반 할당은 무시
		if (op == OP_DBG_IGNORE) {
			return TRUE;
		}

		block_allocated(size, file_name, extracted_line_number, request_number, op);
	} else if (alloc_type == _HOOK_FREE) {
		heap_block_header* block_header = (heap_block_header*)user_data - 1;
		block_deallocated(block_header);
	}

	return TRUE;
}



int main() {
	_CrtSetAllocHook(IGotYou);	// 캡쳐 시작

	int* _1 = dbg_new int[2];
	int* _2 = dbg_new int;
	int* _3 = dbg_new int;
	int* _4 = dbg_new int;
	void* _5 = dbg_operator_new(20);
	void* _6 = dbg_malloc(50);
	int* _7;

	std::thread th([&]() {
		_7 = dbg_new int[128];
	});

	th.join();
	// delete _1;
	// delete _2;
	// delete _3;
	// delete _4;
	::operator delete(_5);
	free(_6);
	delete[] _7;
	
	block_diff(true);		// 캡쳐 결과 출력
	auto fnPrevCallBack = _CrtSetAllocHook(NULL); // 캡쳐 종료
	

	return 0;
}
