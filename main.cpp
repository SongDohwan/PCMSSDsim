#include <inttypes.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>

#include "common.h"
#include "HIL.h"
#include "dram.h"

using namespace std;

uint64_t CYCLE_VAL = 0;
bool is_req_comp = false;
#define MAX_NUM_CHANNEL        2
#define MAX_NUM_WAY            2
#define MAX_NUM_BANK           4
#define max(a,b) (((a)>(b))?(a):(b))

typedef struct pcm_array_queue                                   pcm_queue_t;
typedef enum { IDLE, ACT, COL_READ, COL_WRITE, PRE }     state_t;
typedef struct mem_request_packet                                        mem_req_t;

typedef enum
{
	ACT_CMD,
	COL_READ_CMD,
	COL_WRITE_CMD,
	PRE_CMD,
	REF_CMD,
	NOP
} command_t;

typedef struct pcm_array
{
	uint64_t        total_size;

	uint32_t            n_channel;
	uint32_t            n_way;
	uint32_t        n_bank;
	uint32_t        n_row;
	uint32_t        n_column;

	uint32_t        tRCD;
	uint32_t        tRP;
	uint32_t        tCAS;
	uint32_t        tRC;
	uint32_t        tRAS;
	uint32_t            tBL;

	uint64_t        next_pre[MAX_NUM_CHANNEL][MAX_NUM_WAY][MAX_NUM_BANK];
	uint64_t        next_act[MAX_NUM_CHANNEL][MAX_NUM_WAY][MAX_NUM_BANK];
	uint64_t        next_read[MAX_NUM_CHANNEL][MAX_NUM_WAY][MAX_NUM_BANK];
	uint64_t        next_write[MAX_NUM_CHANNEL][MAX_NUM_WAY][MAX_NUM_BANK];
	state_t         bank_state[MAX_NUM_CHANNEL][MAX_NUM_WAY][MAX_NUM_BANK];
	uint32_t        row_buffer_act[MAX_NUM_CHANNEL][MAX_NUM_WAY][MAX_NUM_BANK];


	uint64_t            data_bus[MAX_NUM_CHANNEL];

	pcm_queue_t     *reqQ;
} pcm_array_t;

struct pcm_array_queue
{
	uint32_t        capacity;
	uint32_t        n_req;
	mem_req_t       *_head;
	mem_req_t       *_tail;
	pcm_array_t     *_pcm_array;
	uint64_t        comp_time;
};

struct mem_request_packet
{
	//? general information of memory request packet
	op_type         req_type;
	uint64_t        phys_addr;      //? actual physical address.
	
	uint32_t        core_id;
	command_t       next_command;
	bool                        cmd_issuable;
	bool                        req_served;
	bool                        row_buffer_miss;
	//? memory device address::
	uint32_t            channel;
	uint32_t            way;
	uint32_t        bank;
	uint64_t        row;
	uint32_t        column;
	mem_req_t*      _next;
};

typedef struct internal_queue
{
	uint32_t        capacity;
	uint32_t        n_req;
	mem_req_t       *_head;
	mem_req_t       *_tail;
	pcm_array_t     *_pcm_array;
	uint64_t        comp_time;
} internal_queue_t;

pcm_queue_t*    init_memory_request_queue(pcm_array_t* pcm_array, uint32_t capacity);
void                    mem_txq_insert_req(pcm_queue_t* _queue, mem_req_t* _req);
void                    mem_txq_remove_req(pcm_queue_t* _queue, mem_req_t* _req);
bool                    is_req_queue_full(pcm_queue_t*   _queue);
bool                    is_req_queue_empty(pcm_queue_t* _queue);
void                    mem_address_scheme(pcm_array_t* _pcm, mem_req_t* _req);
internal_queue_t*    init_internal_queue(pcm_array_t* pcm_array, uint32_t capacity);
void                    internal_insert_req(internal_queue_t* _queue, mem_req_t* _req);
void                    internal_remove_req(internal_queue_t* _queue, mem_req_t* _req);
bool                    is_internal_queue_full(internal_queue_t*   _queue);
bool                    is_internal_empty(internal_queue_t* _queue);

uint32_t u32_log2(uint32_t n)
{
	uint32_t power;
	for (power = 0; n >>= 1; ++power);
	return power;
}

uint64_t u64_log2(uint64_t n)
{
	uint64_t power;
	for (power = 0; n >>= 1; ++power);
	return power;
}

pcm_queue_t* init_memory_request_queue(pcm_array_t* _pcm_array, uint32_t capacity)
{
	pcm_queue_t* new_queue = (pcm_queue_t*)malloc(sizeof(pcm_queue_t));
	new_queue->capacity = capacity;
	new_queue->_pcm_array = _pcm_array;
	new_queue->n_req = 0;
	new_queue->_head = NULL;
	new_queue->_tail = NULL;
	new_queue->comp_time = 0;
	return new_queue;
}

void mem_txq_insert_req(pcm_queue_t* _queue, mem_req_t* _req)
{
	mem_req_t* new_req = (mem_req_t*)malloc(sizeof(mem_req_t));
	memcpy(new_req, _req, sizeof(mem_req_t));
	new_req->_next = NULL;
	if (_queue->n_req == 0)
	{
		_queue->_head = new_req;
		_queue->_tail = new_req;
	}
	else
	{
		_queue->_tail->_next = new_req;
		_queue->_tail = new_req;
	}
	_queue->n_req++;
	mem_address_scheme(_queue->_pcm_array, new_req);
}

void mem_txq_remove_req(pcm_queue_t* _queue, mem_req_t* _req)
{
	mem_req_t *it;
	mem_req_t *prev, *next;
	prev = NULL;
	it = _queue->_head;

	if (_queue->n_req == 1)
	{
		_queue->_head = NULL;
		_queue->_tail = NULL;
		_queue->n_req = 0;
		free(_req);
		return;
	}

	if (_req == _queue->_head)
	{
		_queue->_head = _req->_next;
		_queue->n_req--;
		free(_req);
		return;
	}

	for (uint32_t i = 0; i < _queue->n_req; i++)
	{
		if (it->_next == _req)
		{
			prev = it;
			break;
		}
		if (it == _queue->_tail)
		{
			printf("Queue Error");
			return;
		}
		it = it->_next;
	}
	next = _req->_next;
	if (_req == _queue->_tail)
	{
		prev->_next = NULL;
		_queue->_tail = prev;
		_queue->n_req--;
		free(_req);
		return;
	}
	prev->_next = next;
	_queue->n_req--;
	free(_req);
}

bool is_req_queue_full(pcm_queue_t* _queue)
{
	return (_queue->capacity == _queue->n_req);
}

bool is_req_queue_empty(pcm_queue_t* _queue)
{
	return (_queue->n_req == 0);
}

internal_queue_t* init_internal_queue(pcm_array_t* pcm_array, uint32_t capacity)
{
	internal_queue_t* new_queue = (internal_queue_t*)malloc(sizeof(internal_queue_t*));
	new_queue->capacity = capacity;
	new_queue->_pcm_array = pcm_array;
	new_queue->n_req = 0;
	new_queue->_head = NULL;
	new_queue->_tail = NULL;
	new_queue->comp_time = 0;
	return new_queue;
}

void internal_insert_req(internal_queue_t* _queue, mem_req_t* _req)
{
	mem_req_t* new_req = (mem_req_t*)malloc(sizeof(mem_req_t));
	memcpy(new_req, _req, sizeof(mem_req_t));
	new_req->_next = NULL;
	if (_queue->n_req == 0)
	{
		_queue->_head = new_req;
		_queue->_tail = new_req;
	}
	else
	{
		_queue->_tail->_next = new_req;
		_queue->_tail = new_req;
	}
	_queue->n_req++;
	mem_address_scheme(_queue->_pcm_array, new_req);
}
void internal_remove_req(internal_queue_t* _queue, mem_req_t* _req)
{
	mem_req_t *it;
	mem_req_t *prev, *next;
	prev = NULL;
	it = _queue->_head;

	if (_queue->n_req == 1)
	{
		_queue->_head = NULL;
		_queue->_tail = NULL;
		_queue->n_req = 0;
		free(_req);
		return;
	}

	if (_req == _queue->_head)
	{
		_queue->_head = _req->_next;
		_queue->n_req--;
		free(_req);
		return;
	}

	for (uint32_t i = 0; i < _queue->n_req; i++)
	{
		if (it->_next == _req)
		{
			prev = it;
			break;
		}
		if (it == _queue->_tail)
		{
			printf("Queue Error");
			return;
		}
		it = it->_next;
	}
	next = _req->_next;
	if (_req == _queue->_tail)
	{
		prev->_next = NULL;
		_queue->_tail = prev;
		_queue->n_req--;
		free(_req);
		return;
	}
	prev->_next = next;
	_queue->n_req--;
	free(_req);
}
bool is_internal_queue_full(internal_queue_t*   _queue)
{
	return (_queue->capacity == _queue->n_req);
}
bool is_internal_empty(internal_queue_t* _queue)
{
	return (_queue->n_req == 0);
}

pcm_array_t* init_pcm_array_structure
(
	uint64_t total_size,
	uint32_t n_channel,
	uint32_t n_way,
	uint32_t n_bank,
	uint32_t n_row,
	uint32_t n_column,
	uint32_t tRCD,
	uint32_t tRP,
	uint32_t tCAS,
	uint32_t tRC,
	uint32_t tRAS,
	uint32_t tBL
)
{
	pcm_array_t* new_pcm = (pcm_array_t*)malloc(sizeof(pcm_array_t));
	new_pcm->total_size = total_size;
	new_pcm->n_channel = n_channel;
	new_pcm->n_way = n_way;
	new_pcm->n_bank = n_bank;
	new_pcm->n_row = n_row;
	new_pcm->n_column = n_column;

	new_pcm->reqQ = init_memory_request_queue(new_pcm, 16);

	new_pcm->tRCD = tRCD;
	new_pcm->tRP = tRP;
	new_pcm->tCAS = tCAS;
	new_pcm->tRAS = tRAS;
	new_pcm->tRC = tRC;
	new_pcm->tBL = tBL;

	for (uint32_t i = 0; i < MAX_NUM_CHANNEL; i++)
	{
		for (uint32_t j = 0; j < MAX_NUM_WAY; j++)
		{
			for (uint32_t k = 0; k < MAX_NUM_BANK; k++)
			{
				new_pcm->bank_state[i][j][k] = IDLE;
				new_pcm->row_buffer_act[i][j][k] = UINT64_MAX;
				new_pcm->next_pre[i][j][k] = 0;
				new_pcm->next_act[i][j][k] = 0;
				new_pcm->next_read[i][j][k] = 0;
				new_pcm->next_write[i][j][k] = 0;
				
			}
		}

		new_pcm->data_bus[i] = 0;
	}
	return new_pcm;
}

void mem_address_scheme(pcm_array_t* _pcm, mem_req_t* _req)
{
	uint64_t phys_addr = _req->phys_addr;
	uint64_t temp_addr1, temp_addr2;
	uint64_t part_addr;
	uint32_t channel_bit_width = u32_log2(_pcm->n_channel);
	uint32_t way_bit_width = u32_log2(_pcm->n_way);
	uint32_t bank_bit_width = u32_log2(_pcm->n_bank);
	uint32_t row_bit_width = (uint32_t)u64_log2(_pcm->n_row);
	uint32_t col_bit_width = u32_log2(_pcm->n_column);
	uint32_t byteoffset_width = 6;

	part_addr = phys_addr;
	part_addr = phys_addr >> byteoffset_width;

	temp_addr2 = part_addr;
	part_addr = part_addr >> col_bit_width;
	temp_addr1 = part_addr << col_bit_width;
	_req->column = temp_addr1 ^ temp_addr2;

	temp_addr2 = part_addr;
	part_addr = part_addr >> row_bit_width;
	temp_addr1 = part_addr << row_bit_width;
	_req->row = temp_addr1 ^ temp_addr2;

	temp_addr2 = part_addr;
	part_addr = part_addr >> bank_bit_width;
	temp_addr1 = part_addr << bank_bit_width;
	_req->bank = temp_addr1 ^ temp_addr2;

	temp_addr2 = part_addr;
	part_addr = part_addr >> way_bit_width;
	temp_addr1 = part_addr << way_bit_width;
	_req->way = temp_addr1 ^ temp_addr2;

	temp_addr2 = part_addr;
	part_addr = part_addr >> channel_bit_width;
	temp_addr1 = part_addr << channel_bit_width;
	_req->channel = temp_addr1 ^ temp_addr2;
}

mem_req_t* init_request(uint64_t addr, op_type op)
{
	mem_req_t* new_req = (mem_req_t*)malloc(sizeof(mem_req_t));
	new_req->req_type = op;
	new_req->phys_addr = addr;
	new_req->next_command = NOP;
	new_req->_next = NULL;
	return new_req;
}

void queue_commands(pcm_array_t* _pcm, uint64_t _clk, dram_buffer_t* buffer, internal_queue_t* internal)
{
	mem_req_t* curr = internal->_head;
	while (curr != NULL)
	{

		uint32_t channel = curr->channel;
		uint32_t way = curr->way;
		uint32_t bank = curr->bank;
		uint64_t row = curr->row;
		uint64_t addr = curr->phys_addr;
		/*printf("op: ");
		cout << curr->req_type;
		printf("\ncycle: ");
		cout << _clk;
		printf("\nchannel: ");
		cout << channel;
		printf("\nway: ");
		cout << way;
		printf("\nbank: ");
		cout << bank;
		printf(", row: ");
		cout << row;
		printf(", col: ");
		cout << curr->column;
		printf("\nstate: ");
		cout << _pcm->bank_state[channel][way][bank];
		printf("\n");
		printf("\nact row: ");
		cout << _pcm->row_buffer_act[channel][way][bank];
		printf("\n\n");*/
		if (channel >= _pcm->n_channel || way >= _pcm->n_way || bank >= _pcm->n_bank || row >= _pcm->n_row || curr->column >= _pcm->n_column)
		{
			printf("ERROR");
			return;
		}
		//printf("channel: %u, way: %u, bank: %u, row: %lu\n", channel, way, bank, row);
		buffer_table_t* table = buffer->buffer_table;
		if (table->address_table[addr%BUF_SIZE] == addr)
		{
			curr->next_command = NOP;
			return;
		}

		switch (_pcm->bank_state[channel][way][bank])
		{
			
			//*
		case IDLE:
			_pcm->row_buffer_act[channel][way][bank] = row;
			_pcm->next_pre[channel][way][bank] = _clk + _pcm->tRC+ buffer->dram_latency;
			_pcm->next_act[channel][way][bank] = _clk + _pcm->tRCD;
			_pcm->next_read[channel][way][bank] = _clk + _pcm->tRCD + _pcm->tCAS;
			_pcm->next_write[channel][way][bank] = _clk + _pcm->tRCD + _pcm->tCAS;


			curr->next_command = ACT_CMD;
			//else if (curr->req_type == WRITE)
			break;

		case ACT:
			if (curr->req_type == READ)
				curr->next_command = COL_READ_CMD;
			else
				curr->next_command = COL_WRITE_CMD;
			break;
		case COL_READ:
			curr->next_command = PRE_CMD;
			break;

		case COL_WRITE:
			curr->next_command = PRE_CMD;
			break;

		case PRE:
			curr->next_command = NOP;
			break;
		default:        break;
		}
		curr = curr->_next;
	}
}

void update(pcm_array_t* _pcm, uint64_t _clk, dram_buffer_t* buffer, internal_queue_t* internal)
{
	mem_req_t* curr = internal->_head;
	/*cout << "cycle:";
	cout << _clk;
	cout << "\n";*/
	while (curr != NULL)
	{
		uint32_t channel = curr->channel;
		uint32_t way = curr->way;
		uint32_t bank = curr->bank;
		uint64_t row = curr->row;
		uint64_t addr = curr->phys_addr;
		buffer_table_t* table = buffer->buffer_table;
		
		switch (curr->next_command)
		{
		case ACT_CMD:
			_pcm->bank_state[channel][way][bank] = ACT;
			curr = curr->_next;
			break;

		case COL_READ_CMD:
			if (_pcm->next_act[channel][way][bank] <= _clk)
			{
				_pcm->bank_state[channel][way][bank] = COL_READ;
			}
			curr = curr->_next;
			break;
		case COL_WRITE_CMD:
			if (_pcm->next_act[channel][way][bank] <= _clk)
			{
				_pcm->bank_state[channel][way][bank] = COL_WRITE;
			}
			curr = curr->_next;
			break;

		case PRE_CMD:
			if (curr->req_type == READ)
			{
				if (_pcm->next_read[channel][way][bank] <= _clk)
					_pcm->bank_state[channel][way][bank] = PRE;
			}
			else
			{
				if (_pcm->next_write[channel][way][bank] <= _clk)
					_pcm->bank_state[channel][way][bank] = PRE;
			}
			curr = curr->_next;
			break;

		case NOP:
			
			/*cout << "channel: ";
			cout << curr->channel;
			cout << ",   way: ";
			cout << curr->way;
			cout << "\n";*/
			
				
			
			if (table->address_table[addr%BUF_SIZE] == addr)
			{
				/*cout << "hit\n";
				//printf("HIT\n");
				// buffer->dram_latency;
				cout << CYCLE_VAL + buffer->dram_latency;
				cout << "\n";
				cout << _pcm->reqQ->comp_time;
				cout << "\n";*/
				_pcm->reqQ->comp_time = max(CYCLE_VAL+buffer->dram_latency, _pcm->reqQ->comp_time);
				mem_req_t* del = curr;
				curr = curr->_next;
				internal_remove_req(internal, del);
				is_req_comp = true;
				
			}
			else if (_pcm->next_pre[channel][way][bank] <= _clk)
			{
				_pcm->bank_state[channel][way][bank] = IDLE;
				if (_pcm->data_bus[channel] <= _clk)
					_pcm->data_bus[channel] = _clk + _pcm->tBL;
				else
					_pcm->data_bus[channel] = _pcm->data_bus[channel] + _pcm->tBL;

				if (channel == 0)
				{
					_pcm->reqQ->comp_time = max(_pcm->data_bus[channel], _pcm->data_bus[1]);
				}
				else if (channel == 1)
				{
					_pcm->reqQ->comp_time = max(_pcm->data_bus[0], _pcm->data_bus[channel]);
				}
				insert_buffer(table, addr);
				mem_req_t* del = curr;
				
				curr = curr->_next;
				internal_remove_req(internal, del);
				/*cout << "table: ";
				cout << table->address_table[addr%BUF_SIZE];
				cout << "\n";
				cout << internal->n_req;
				cout << "\n";*/
				is_req_comp = true;
			}
			else
				curr = curr->_next;

			break;

		default:        break;
		}
	}
}


FILE *trace;
#pragma warning(disable : 4996)

int main(int argc, char** argv)
{
	int end = 0;
	mem_req_t*   _req;
	uint64_t     _addr;
	op_type      _type;
	buffer_table_t* table = init_buffer_table();
	dram_buffer_t* buffer = init_dram_buffer(table, 10);
	pcm_array_t* _pcm = init_pcm_array_structure(64, 2, 2, 4, 4, 8, 11, 11, 11, 39, 28, 8);
	internal_queue_t* internal = init_internal_queue(_pcm, MAX_NUM_CHANNEL*MAX_NUM_WAY);

	//trace = fopen(argv[1], "r");
	trace = fopen("C:\\Users\\User\\source\\repos\\Project2\\Project2\\in.trc", "r");

	/*
	mem_req_t* _req = init_request(640 ,READ);
	mem_txq_insert_req(_pcm->reqQ, _req);
	_req = init_request(640, WRITE);
	mem_txq_insert_req(_pcm->reqQ, _req);
	_req = init_request(1664, WRITE);
	mem_txq_insert_req(_pcm->reqQ, _req);
	*/
	while (1)
	{
		//* Now, we get I/O requests from trace file.
		//* - format: [0/1:READ/WRITE] [BLOCK_ADDRESS]
		//* (reading a line per a cycle)
		if(!feof(trace))
		{
			fscanf(trace, "%u %llu\n", &_type, &_addr);
			//printf("%u %llu\n", _type, _addr);
			_req = init_request(_addr, _type);
			mem_txq_insert_req(_pcm->reqQ, _req);
			
		}
		if (internal->n_req != internal->capacity)
		{
			if (internal->n_req == 0)
			{
				internal_insert_req(internal, _pcm->reqQ->_head);
				
				mem_txq_remove_req(_pcm->reqQ, _pcm->reqQ->_head);
			}
			else
			{
				mem_req_t* curr = _pcm->reqQ->_head;
				
				while(curr != NULL)
				{
					if (internal->n_req == internal->capacity)
						break;
					mem_req_t* in_curr = internal->_head;
					int in = 0;
					while(in_curr != NULL)
					{
						if (curr->channel != in_curr->channel || curr->way != in_curr->way)
						{
							in++;
						}
						in_curr = in_curr->_next;
					}
					if (in == internal->n_req)
					{
						internal_insert_req(internal, curr);
						mem_req_t* del = curr;
						curr = curr->_next;
						mem_txq_remove_req(_pcm->reqQ, del);
					}
					else
						curr = curr->_next;
				}
			}
		}
		is_req_comp = false;
		queue_commands(_pcm, CYCLE_VAL, buffer, internal);
		update(_pcm, CYCLE_VAL, buffer, internal);
		end = 1;

		for (int i = 0; i < MAX_NUM_CHANNEL; i++)
			for (int j = 0; j < MAX_NUM_WAY; j++)
				for (int k = 0; k < MAX_NUM_BANK; k++)
				{
					if (_pcm->reqQ->n_req != 0 || internal->n_req != 0 || _pcm->bank_state[i][j][k] != IDLE)
						end = 0;
				}
		
		if (end == 1)
			break;
		if (is_req_comp == true)
			continue;
		CYCLE_VAL++;

	}

	printf("Total Execution Cycles: %llu\n", _pcm->reqQ->comp_time);

	fclose(trace);
	int a;
	cin >> a;
	
	
	
	return 0;
}



dram_buffer_t* init_dram_buffer(buffer_table_t* table, uint32_t latency)
{
	dram_buffer_t* new_buffer = (dram_buffer_t*)malloc(sizeof(dram_buffer_t));
	new_buffer->dram_latency = latency;
	new_buffer->buffer_table = table;
	return new_buffer;
}

buffer_table_t* init_buffer_table()
{
	buffer_table_t* new_table = (buffer_table_t*)malloc(sizeof(buffer_table_t));
	new_table->entry_num = 0;
	for (uint32_t i = 0; i < BUF_SIZE; i++)
	{
		new_table->address_table[i] = UINT64_MAX;
	}
	return new_table;
}

void check_buffer(dram_buffer_t* buffer, uint64_t addr)
{
	buffer_table_t* table = buffer->buffer_table;
	if (table->address_table[addr%BUF_SIZE] == addr)
	{
		printf("HIT\n");
		// buffer->dram_latency;
	}
	else
	{
		printf("MISS\n");
		insert_buffer(table, addr);
	}
}

void insert_buffer(buffer_table_t* table, uint64_t addr)
{
	if (table->address_table[addr%BUF_SIZE] == UINT64_MAX)
	{
		table->address_table[addr%BUF_SIZE] = addr;
		table->entry_num++;
	}
	else
	{
		table->address_table[addr%BUF_SIZE] = addr;
	}
}

void remove_buffer(buffer_table_t* table, uint64_t addr)
{
	if (table->entry_num != 0)
	{
		table->address_table[addr%BUF_SIZE] = UINT64_MAX;
		table->entry_num--;
	}
	else
		printf("ERROR\n");
}