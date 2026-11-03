// license:BSD-3-Clause
// copyright-holders:Angelo Salese, Ariane Fugmann
/***************************************************************************

    Namco C139 - Serial I/F Controller

***************************************************************************/
#ifndef MAME_NAMCO_NAMCO_C139_H
#define MAME_NAMCO_NAMCO_C139_H

#pragma once

#include <deque>
#include <vector>


//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// ======================> namco_c139_device

class namco_c139_device : public device_t
{
public:
	// construction/destruction
	namco_c139_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	auto irq_cb() { return m_irq_cb.bind(); }

	// I/O operations
	void data_map(address_map &map) ATTR_COLD;
	void regs_map(address_map &map) ATTR_COLD;

	uint16_t ram_r(offs_t offset);
	void ram_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);

	uint16_t reg_r(offs_t offset);
	void reg_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);

	// boards of one machine wired together (Driver's Eyes): what this chip sends goes to 'peer' instead of the network
	void add_local_peer(namco_c139_device &peer);

protected:
	// device-level overrides
	virtual void device_start() override ATTR_COLD;
	virtual void device_stop() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

	devcb_write_line m_irq_cb;

private:
	class context;

	uint16_t m_ram[0x2000];
	uint16_t m_reg[8];

	bool m_tx_busy;
	bool m_rx_locked;
	attotime m_rx_irq_time;

	std::string m_localhost;
	std::string m_localport;
	std::string m_remotehost;
	std::string m_remoteport;

	std::unique_ptr<context> m_context;
	std::vector<namco_c139_device *> m_local_peers;
	std::deque<std::vector<uint16_t> > m_local_rx;

	emu_timer *m_tick_timer;
	emu_timer *m_tx_timer;

	TIMER_CALLBACK_MEMBER(tick_timer_callback);
	TIMER_CALLBACK_MEMBER(tx_done);

	void check_tx();
	void check_rx();
	void update_irq();
	void deliver_local(std::vector<uint16_t> const &words);
};


// device type definition
DECLARE_DEVICE_TYPE(NAMCO_C139, namco_c139_device)


#endif // MAME_NAMCO_NAMCO_C139_H
