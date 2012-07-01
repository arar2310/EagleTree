
#include "ssd.h"

using namespace ssd;

Block_manager_parallel_hot_cold_seperation::Block_manager_parallel_hot_cold_seperation(Ssd& ssd, FtlParent& ftl)
	: Block_manager_parent(ssd, ftl),
	  page_hotness_measurer()
{
	cold_pointer = find_free_unused_block(0, 0);
}

Block_manager_parallel_hot_cold_seperation::~Block_manager_parallel_hot_cold_seperation(void) {}

void Block_manager_parallel_hot_cold_seperation::register_write_outcome(Event const& event, enum status status) {
	assert(event.get_event_type() == WRITE);
	if (status == FAILURE) {
		return;
	}
	Block_manager_parent::register_write_outcome(event, status);
	if (!event.is_garbage_collection_op()) {
		page_hotness_measurer.register_event(event);
	}

	// Increment block pointer
	uint package_id = event.get_address().package;
	uint die_id = event.get_address().die;
	Address block_address = Address(event.get_address().get_linear_address(), BLOCK);
	uint num_pages_written = -1;
	if (block_address.compare(free_block_pointers[package_id][die_id]) == BLOCK) {
		Address pointer = free_block_pointers[package_id][die_id];
		pointer.page = num_pages_written = pointer.page + 1;
		free_block_pointers[package_id][die_id] = pointer;
	}
	else if (block_address.compare(cold_pointer) == BLOCK) {
		cold_pointer.page = num_pages_written = cold_pointer.page + 1;
	}

	// there is still more room in this pointer, so no need to trigger GC
	if (num_pages_written < BLOCK_SIZE) {
		return;
	}

	// check if the pointer if full. If it is, find a free block for a new pointer, or trigger GC if there are no free blocks
	if (block_address.compare(free_block_pointers[package_id][die_id]) == BLOCK) {
		printf("hot pointer ");
		free_block_pointers[package_id][die_id].print();
		printf(" is out of space\n");
		Address free_block = find_free_unused_block(package_id, die_id);
		if (free_block.valid != NONE) {
			free_block_pointers[package_id][die_id] = free_block;
		} else {
			Garbage_Collect(package_id, die_id, event.get_start_time() + event.get_time_taken());
		}
	} else if (block_address.compare(cold_pointer) == BLOCK) {
		handle_cold_pointer_out_of_space(event.get_start_time() + event.get_time_taken());
	}
}

void Block_manager_parallel_hot_cold_seperation::handle_cold_pointer_out_of_space(double start_time) {
	Address free_block = find_free_unused_block();
	if (free_block.valid != NONE) {
		cold_pointer = free_block;
	} else {
		perform_emergency_garbage_collection(start_time);
	}
}

void Block_manager_parallel_hot_cold_seperation::register_erase_outcome(Event const& event, enum status status) {
	assert(event.get_event_type() == ERASE);
	if (status == FAILURE) {
		return;
	}
	Block_manager_parent::register_erase_outcome(event, status);
	uint package_id = event.get_address().package;
	uint die_id = event.get_address().die;

	Address addr = event.get_address();
	addr.valid = PAGE;
	addr.page = 0;

	// TODO: Need better logic for this assignment. Easiest to remember some state.
	// when we trigger GC for a cold pointer, remember which block was chosen.
	if (free_block_pointers[package_id][die_id].page >= BLOCK_SIZE) {
		free_block_pointers[package_id][die_id] = addr;
	}
	else if (cold_pointer.page >= BLOCK_SIZE) {
		cold_pointer = addr;
	}

	check_if_should_trigger_more_GC(event.get_start_time() + event.get_time_taken());
	Wear_Level(event);
}

// ensures the pointer has at least 1 free page, and that the die is not busy (waiting for a read)
bool Block_manager_parallel_hot_cold_seperation::pointer_can_be_written_to(Address pointer) const {
	bool has_space = pointer.page < BLOCK_SIZE;
	bool non_busy = !ssd.getPackages()[pointer.package].getDies()[pointer.die].register_is_busy();
	return has_space && non_busy;
}


bool Block_manager_parallel_hot_cold_seperation::at_least_one_available_write_hot_pointer() const  {
	for (uint i = 0; i < SSD_SIZE; i++) {
		for (uint j = 0; j < PACKAGE_SIZE; j++) {
			if (pointer_can_be_written_to(free_block_pointers[i][j])) {
				return true;
			}
		}
	}
	return false;
}


/*
 * makes sure that there is at least 1 non-busy die with free space
 * and that the die is not waiting for an impending read transfer
 */
bool Block_manager_parallel_hot_cold_seperation::can_write(Event const& write) const {
	if (!Block_manager_parent::can_write(write)) {
		return false;
	}

	bool hot_pointer_available = at_least_one_available_write_hot_pointer();
	bool cold_pointer_available = pointer_can_be_written_to(cold_pointer);

	if (write.is_garbage_collection_op()) {
		return hot_pointer_available || cold_pointer_available;
	}

	// left with norm
	enum write_hotness w_hotness = page_hotness_measurer.get_write_hotness(write.get_logical_address());

	if (w_hotness == WRITE_HOT) {
		return hot_pointer_available;
	} else {
		return cold_pointer_available;
	}
	assert(false);
	return false;
}

void Block_manager_parallel_hot_cold_seperation::register_read_outcome(Event const& event, enum status status){
	if (status == SUCCESS && !event.is_garbage_collection_op()) {
		page_hotness_measurer.register_event(event);
	}
}

void Block_manager_parallel_hot_cold_seperation::check_if_should_trigger_more_GC(double start_time) {
	Block_manager_parent::check_if_should_trigger_more_GC(start_time);
	if (cold_pointer.page >= BLOCK_SIZE) {
		handle_cold_pointer_out_of_space(start_time);
	}
}

Address Block_manager_parallel_hot_cold_seperation::choose_write_location(Event const& event) const {
	// if GC, try writing in appropriate pointer. If that doesn't work, write anywhere free.
	enum write_hotness w_hotness = page_hotness_measurer.get_write_hotness(event.get_logical_address());
	bool wh_available = at_least_one_available_write_hot_pointer();

	// TODO: if write-hot, need to assign READ_HOT to non-busy planes and READ_COLD to busy planes. Do this while still trying to write to a die with a short queue
	if (wh_available && w_hotness == WRITE_HOT) {
		return get_free_die_with_shortest_IO_queue();
	}

	bool cold_pointer_available = pointer_can_be_written_to(cold_pointer);

	if (cold_pointer_available && w_hotness == WRITE_COLD) {
		printf("WRITE_COLD\n");
		return cold_pointer;
	}

	printf("MISTAKE\n");
	// if we are here, we must make a mistake. Simply choose some free pointer.
	// can only get here if can_write returned true. It only allows mistakes for GC
	assert(event.is_garbage_collection_op());
	return wh_available ? get_free_die_with_shortest_IO_queue() : cold_pointer;
}
