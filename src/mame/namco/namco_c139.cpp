// license:BSD-3-Clause
// copyright-holders:Angelo Salese, John Bennett, Ariane Fugmann
/***************************************************************************

    Namco C139 - Serial I/F Controller

    (from assault schematics, page 5-18 and 5-19)
    connected to M5M5179P RAM with a 13-bit address bus, and 9 bit data bus
    connected to host cpu with a 14*-bit address bus, and 13 bit data bus
    2 clock inputs - 16M and 12M

    Used on System 2, System 21, System 22 / Super System 22 to link cabinets in a
    ring (each chip transmits to the next one) and, in Driver's Eyes, the three
    boards of one machine. The model below comes from the link code of the games
    (see C139.md at the top of the source tree for the per-game analysis).

    RAM: 0x2000 9-bit words. Words 0x0000-0x0fff are the transmit area, words
    0x1000-0x1fff the receive ring. Bit 8 is the frame sync bit: the games set it
    on the last word of a frame and look for it when parsing received frames.

    Registers (Namco names from the Cyber Cycles COMMUNICATION screen):
    0 IRQ   (STATUS)   bit 0 error, bit 1 frame received, bit 2 transmitter idle
                       (live, set after reset); writing clears bits 0-1 where the
                       written value has a 0
    1 MSK   (MODE)     interrupt masks, 1 = masked: bit 1 receive, bit 2 transmitter
                       idle; bit 3 must be set for any interrupt (winrun writes 0 and
                       never acknowledges). When the chip raises an interrupt it sets
                       the corresponding mask bit, the game clears it to re-arm. The
                       "modes" 0x08/0x09/0x0c/0x0d/0x0f of the old notes are masks.
    2 B-9   (CONTROL)  forces bit 8 of host writes to the RAM: 1 = clear, 3 = set
                       (ridgera2 and raverace write their frames with 1, then the
                       checksum, last word of the frame, with 3)
    3 S-M   (START)    bit 0 holds the transmitter while the game programs it
    4 R-C   (RXSIZE)   decremented for each word received (Final Lap checks it)
    5 S-C   (TXSIZE)   words to send; a transmission starts when it is not 0 and
                       START bit 0 is clear, it reads 0 again once sent
    6 R-AD  (RXOFFSET) next receive address, words go to 0x1000 | (RXOFFSET & 0xfff)
    7 S-AD  (TXOFFSET) 13-bit word address of the next word to send; the games relay
                       received frames themselves by pointing it into the receive
                       ring (0x1000 + position)

    The chip doesn't relay, filter or modify anything: every frame sent arrives in
    the next chip's receive ring, and the games forward, count and remove frames
    themselves (hop counters in the frames). Only one frame is put in the receive
    ring per receive interrupt: the handlers only parse the last frame before
    RXOFFSET.

    TODO:
    - hook a real chip and test in detail (START config values 3 / 0x11, error bit,
      line speed, whether a transmission stops at a word with bit 8)
    - C422 seems to be a pin compatible upgrade to C139, probably supporting higher clock speeds? hook up c139 in system23

***************************************************************************/

#include "emu.h"
#include "namco_c139.h"

#include "emuopts.h"

#include "asio.h"

#include <iostream>

#define VERBOSE 0
#include "logmacro.h"


class namco_c139_device::context
{
public:
	context(namco_c139_device &device) :
		m_device(device),
		m_acceptor(m_ioctx),
		m_sock_rx(m_ioctx),
		m_sock_tx(m_ioctx),
		m_timeout_tx(m_ioctx),
		m_stopping(false),
		m_state_rx(0U),
		m_state_tx(0U),
		m_sending(false),
		m_rx_discard(UINT_MAX)
	{
	}

	void start()
	{
		m_thread = std::thread(
				[this] ()
				{
					LOG("C139: network thread started\n");
					try {
						m_ioctx.run();
					} catch (const std::exception& e) {
						LOG("C139: Exception in network thread: %s\n", e.what());
					} catch (...) { // Catch any other unknown exceptions
						LOG("C139: Unknown exception in network thread\n");
					}
					LOG("C139: network thread completed\n");
				});
	}

	void reset(std::string localhost, std::string localport, std::string remotehost, std::string remoteport)
	{
		std::error_code err;
		asio::ip::tcp::resolver resolver(m_ioctx);

		for (auto &&resolveIte : resolver.resolve(localhost, localport, asio::ip::tcp::resolver::flags::address_configured, err))
		{
			m_localaddr = resolveIte.endpoint();
			LOG("C139: localhost = %s\n", *m_localaddr);
		}
		if (err)
		{
			LOG("C139: localhost resolve error: %s\n", err.message());
		}

		for (auto &&resolveIte : resolver.resolve(remotehost, remoteport, asio::ip::tcp::resolver::flags::address_configured, err))
		{
			m_remoteaddr = resolveIte.endpoint();
			LOG("C139: remotehost = %s\n", *m_remoteaddr);
		}
		if (err)
		{
			LOG("C139: remotehost resolve error: %s\n", err.message());
		}

		asio::post(m_ioctx, 
				[this] ()
				{
					std::error_code err;
					if (m_acceptor.is_open())
						m_acceptor.close(err);
					if (m_sock_rx.is_open())
						m_sock_rx.close(err);
					if (m_sock_tx.is_open())
						m_sock_tx.close(err);
					m_timeout_tx.cancel();
					m_state_rx.store(0);
					m_state_tx.store(0);
					// what was queued for or received from the old connections is stale
					m_fifo_tx.consume(m_fifo_tx.used());
					m_rx_discard.store(m_fifo_rx.write_position(), std::memory_order_release);
					start_accept();
					start_connect();
				});
	}

	void stop()
	{
		asio::post(m_ioctx, 
				[this] ()
				{
					m_stopping = true;
					std::error_code err;
					if (m_acceptor.is_open())
						m_acceptor.close(err);
					if (m_sock_rx.is_open())
						m_sock_rx.close(err);
					if (m_sock_tx.is_open())
						m_sock_tx.close(err);
					m_timeout_tx.cancel();
					m_state_rx.store(0);
					m_state_tx.store(0);
					m_ioctx.stop();
				});
		m_work_guard.reset();
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

	bool connected()
	{
		return m_state_rx.load() == 2 && m_state_tx.load() == 2;
	}

	// a frame on the wire: 16-bit word count, then each 9-bit word as 2 bytes, little-endian
	bool receive(std::vector<uint16_t> &words)
	{
		// drop what was left over from a lost connection (the network thread only marks it)
		unsigned const discard = m_rx_discard.exchange(UINT_MAX, std::memory_order_acquire);
		if (discard != UINT_MAX)
			m_fifo_rx.discard_to(discard);

		if (m_state_rx.load() < 2)
			return false;

		uint8_t header[2];
		if (m_fifo_rx.used() < 2 || m_fifo_rx.read(&header[0], 2, true) != 2)
			return false;
		unsigned const count = header[0] | (header[1] << 8);
		if (m_fifo_rx.used() < 2 + count * 2)
			return false;

		std::vector<uint8_t> data(2 + count * 2);
		m_fifo_rx.read(&data[0], data.size(), false);
		words.resize(count);
		for (unsigned i = 0; i < count; i++)
			words[i] = (data[2 + i * 2] | (data[3 + i * 2] << 8)) & 0x1ff;
		return true;
	}

	void send(std::vector<uint16_t> const &words)
	{
		if (m_state_tx.load() < 2)
			return;

		std::vector<uint8_t> data(2 + words.size() * 2);
		data[0] = words.size() & 0xff;
		data[1] = (words.size() >> 8) & 0xff;
		for (unsigned i = 0; i < words.size(); i++)
		{
			data[2 + i * 2] = words[i] & 0xff;
			data[3 + i * 2] = (words[i] >> 8) & 0xff;
		}

		if (data.size() > m_fifo_tx.free())
		{
			LOG("C139: TX buffer overflow\n");
			return;
		}

		m_fifo_tx.write(&data[0], data.size());
		if (!m_sending.exchange(true))
			asio::post(m_ioctx,
					[this] ()
					{
						start_send_tx();
					});
	}

private:
	class fifo
	{
	public:
		fifo() :
			m_wp(0),
			m_rp(0)
		{
		}

		unsigned write(uint8_t* buffer, unsigned data_size)
		{
			unsigned current_rp = m_rp.load(std::memory_order_acquire);
			unsigned current_wp = m_wp.load(std::memory_order_relaxed);

			// calculate free space
			unsigned data_free = (BUFFER_SIZE + current_rp - current_wp - 1) % BUFFER_SIZE;

			// sanity checks
			if (data_size > data_free)
				return UINT_MAX;
			if (data_size == 0)
				return 0;

			unsigned data_used = 0;

			// first part (up to end)
			unsigned block = std::min(data_size, BUFFER_SIZE - current_wp);
			std::copy_n(&buffer[0], block, &m_buffer[current_wp]);
			data_used += block;
			current_wp = (current_wp + block) % BUFFER_SIZE;

			// second part (from beginning, if wrapped)
			if (data_used < data_size)
			{
				block = data_size - data_used;
				std::copy_n(&buffer[data_used], block, &m_buffer[current_wp]);
				data_used += block;
				current_wp += block;
			}

			m_wp.store(current_wp, std::memory_order_release);
			return data_used;
		}

		unsigned read(uint8_t* buffer, unsigned data_size, bool peek)
		{
			unsigned current_wp = m_wp.load(std::memory_order_acquire);
			unsigned current_rp = m_rp.load(std::memory_order_relaxed);

			// calculate available data
			unsigned data_avail = (BUFFER_SIZE + current_wp - current_rp) % BUFFER_SIZE;

			// sanity checks
			if (data_size > data_avail)
				return UINT_MAX;
			if (data_size == 0)
				return 0;

			unsigned data_used = 0;

			// first part (up to end)
			unsigned block = std::min(data_size, BUFFER_SIZE - current_rp);
			std::copy_n(&m_buffer[current_rp], block, &buffer[0]);
			data_used += block;
			current_rp = (current_rp + data_used) % BUFFER_SIZE;

			// second part (from beginning, if wrapped)
			if (data_used < data_size)
			{
				block = data_size - data_used;
				std::copy_n(&m_buffer[current_rp], block, &buffer[data_used]);
				data_used += block;
				current_rp += block;
			}
			if (!peek)
			{
				m_rp.store(current_rp, std::memory_order_release);
			}
			return data_used;
		}

		void consume(unsigned data_size)
		{
			unsigned current_wp = m_wp.load(std::memory_order_acquire);
			unsigned current_rp = m_rp.load(std::memory_order_relaxed);

			//  available data
			unsigned data_avail = (BUFFER_SIZE + current_wp - current_rp) % BUFFER_SIZE;

			// sanity check
			if (data_size > data_avail)
				data_size = data_avail;

			current_rp = (current_rp + data_size) % BUFFER_SIZE;
			m_rp.store(current_rp, std::memory_order_release);
		}

		unsigned used()
		{
			unsigned current_wp = m_wp.load(std::memory_order_acquire);
			unsigned current_rp = m_rp.load(std::memory_order_acquire);
			return (BUFFER_SIZE + current_wp - current_rp) % BUFFER_SIZE;
		}

		unsigned free()
		{
			unsigned current_wp = m_wp.load(std::memory_order_acquire);
			unsigned current_rp = m_rp.load(std::memory_order_acquire);
			return (BUFFER_SIZE + current_rp - current_wp - 1 + BUFFER_SIZE) % BUFFER_SIZE;
		}

		// producer side: where the next byte will be written
		unsigned write_position() const
		{
			return m_wp.load(std::memory_order_relaxed);
		}

		// consumer side: drop what was written before a write_position() taken by the producer, unless it was read already
		void discard_to(unsigned position)
		{
			unsigned const current_wp = m_wp.load(std::memory_order_acquire);
			unsigned const current_rp = m_rp.load(std::memory_order_relaxed);
			unsigned const data_avail = (BUFFER_SIZE + current_wp - current_rp) % BUFFER_SIZE;
			if (((BUFFER_SIZE + position - current_rp) % BUFFER_SIZE) <= data_avail)
				m_rp.store(position, std::memory_order_release);
		}

	private:
		static constexpr unsigned BUFFER_SIZE = 0x80000;
		std::atomic<unsigned> m_wp;
		std::atomic<unsigned> m_rp;
		std::array<uint8_t, BUFFER_SIZE> m_buffer;
	};

	void start_accept()
	{
		if (m_stopping)
			return;

		std::error_code err;
		m_acceptor.open(m_localaddr->protocol(), err);
		m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
		if (!err)
		{
			m_acceptor.bind(*m_localaddr, err);
			if (!err)
			{
				m_acceptor.listen(1, err);
				if (!err)
				{
					osd_printf_verbose("C139: RX listen on %s\n", *m_localaddr);
					m_acceptor.async_accept(
							[this] (std::error_code const &err, asio::ip::tcp::socket sock)
							{
								if (err)
								{
									if (err == asio::error::operation_aborted)
										return; // closed on purpose (reset, stop): whoever closed it listens again
									LOG("C139: RX error accepting - %d %s\n", err.value(), err.message());
									std::error_code e;
									m_acceptor.close(e);
									m_state_rx.store(0);
									start_accept();
								}
								else
								{
									LOG("C139: RX connection from %s\n", sock.remote_endpoint());
									std::error_code e;
									m_acceptor.close(e);
									m_sock_rx = std::move(sock);
									m_sock_rx.set_option(asio::socket_base::keep_alive(true));
									m_state_rx.store(2);
									start_receive_rx();
								}
							});
					m_state_rx.store(1);
				}
			}
		}
		if (err)
		{
			LOG("C139: RX failed - %d %s\n", err.value(), err.message());
		}
	}

	void start_connect()
	{
		if (m_stopping)
			return;

		std::error_code err;
		if (m_sock_tx.is_open())
			m_sock_tx.close(err);
		m_sock_tx.open(m_remoteaddr->protocol(), err);
		if (!err)
		{
			m_sock_tx.set_option(asio::ip::tcp::no_delay(true));
			m_sock_tx.set_option(asio::socket_base::keep_alive(true));
			osd_printf_verbose("C139: TX connecting to %s\n", *m_remoteaddr);
			m_timeout_tx.expires_after(std::chrono::seconds(10));
			m_timeout_tx.async_wait(
					[this] (std::error_code const &err)
					{
						if (!err && m_state_tx.load() == 1)
						{
							osd_printf_verbose("C139: TX connect timed out\n");
							std::error_code e;
							m_sock_tx.close(e);
							m_state_tx.store(0);
							start_connect();
						}
					});
			m_sock_tx.async_connect(
					*m_remoteaddr,
					[this] (std::error_code const &err)
					{
						if (err == asio::error::operation_aborted)
							return; // closed on purpose (time-out, reset, stop): whoever closed it connects again
						m_timeout_tx.cancel();
						if (err)
						{
							osd_printf_verbose("C139: TX connect error - %d %s\n", err.value(), err.message());
							std::error_code e;
							m_sock_tx.close(e);
							m_state_tx.store(0);
							start_connect();
						}
						else
						{
							LOG("C139: TX connection established\n");
							m_state_tx.store(2);
							if (m_fifo_tx.used() && !m_sending.exchange(true))
								start_send_tx();
						}
					});
			m_state_tx.store(1);
		}
	}

	void start_send_tx()
	{
		if (m_stopping || (m_state_tx.load() != 2))
		{
			// not connected: the connection handler starts sending again
			m_sending.store(false);
			return;
		}

		unsigned const pending = m_fifo_tx.used();
		if (!pending)
		{
			// nothing left: stop, unless the emulation thread queued data while it still saw us sending
			m_sending.store(false);
			if (m_fifo_tx.used() && !m_sending.exchange(true))
				start_send_tx();
			return;
		}

		unsigned used = m_fifo_tx.read(&m_buffer_tx[0], std::min<unsigned>(pending, m_buffer_tx.size()), true);
		m_sock_tx.async_write_some(
				asio::buffer(&m_buffer_tx[0], used),
				[this] (std::error_code const &err, std::size_t length)
				{
					m_fifo_tx.consume(length);
					if (err)
					{
						m_fifo_tx.consume(m_fifo_tx.used()); // this thread is the reader of the TX fifo
						m_sending.store(false);
						if (err == asio::error::operation_aborted)
							return; // closed on purpose (time-out, reset, stop): whoever closed it connects again
						LOG("C139: TX connection error: %s\n", err.message().c_str());
						std::error_code e;
						m_sock_tx.close(e);
						m_state_tx.store(0);
						start_connect();
					}
					else
					{
						start_send_tx();
					}
				});
	}

	void start_receive_rx()
	{
		if (m_stopping)
			return;

		m_sock_rx.async_read_some(
				asio::buffer(m_buffer_rx),
				[this] (std::error_code const &err, std::size_t length)
				{
					if (err || !length)
					{
						if (err == asio::error::operation_aborted)
							return; // closed on purpose (reset, stop): whoever closed it listens again
						if (err)
							LOG("C139: RX connection error: %s\n", err.message());
						else
							LOG("C139: RX connection lost\n");
						std::error_code e;
						m_sock_rx.close(e);
						m_state_rx.store(0);
						m_rx_discard.store(m_fifo_rx.write_position(), std::memory_order_release);
						start_accept();
					}
					else
					{
						if (UINT_MAX == m_fifo_rx.write(&m_buffer_rx[0], length))
						{
							LOG("C139: RX buffer overflow\n");
							std::error_code e;
							m_sock_rx.close(e);
							m_state_rx.store(0);
							m_rx_discard.store(m_fifo_rx.write_position(), std::memory_order_release);
							start_accept();
							return;
						}
						start_receive_rx();
					}
				});
	}

public:
	// used by LOG(), so it has to be accessible from outside
	template <typename Format, typename... Params>
	void logerror(Format &&fmt, Params &&... args) const
	{
		util::stream_format(
				std::cerr,
				"[%s] %s",
				m_device.tag(),
				util::string_format(std::forward<Format>(fmt), std::forward<Params>(args)...));
	}

private:

	namco_c139_device &m_device;
	std::thread m_thread;
	asio::io_context m_ioctx;
	asio::executor_work_guard<asio::io_context::executor_type> m_work_guard{m_ioctx.get_executor()};
	std::optional<asio::ip::tcp::endpoint> m_localaddr;
	std::optional<asio::ip::tcp::endpoint> m_remoteaddr;
	asio::ip::tcp::acceptor m_acceptor;
	asio::ip::tcp::socket m_sock_rx;
	asio::ip::tcp::socket m_sock_tx;
	asio::steady_timer m_timeout_tx;
	bool m_stopping;
	std::atomic_uint m_state_rx;
	std::atomic_uint m_state_tx;
	std::atomic_bool m_sending;     // a write is in flight or posted on the network thread
	std::atomic_uint m_rx_discard;  // RX fifo write position of a lost connection, UINT_MAX if none
	fifo m_fifo_rx;
	fifo m_fifo_tx;
	std::array<uint8_t, 0x400> m_buffer_rx;
	std::array<uint8_t, 0x400> m_buffer_tx;
};


namespace {

enum : unsigned
{
	REG_STATUS = 0,
	REG_MODE,
	REG_CONTROL,
	REG_START,
	REG_RXSIZE,
	REG_TXSIZE,
	REG_RXOFFSET,
	REG_TXOFFSET
};

// one 9-bit word with start and stop bits at about 2 Mbit/s
attotime const WORD_TIME = attotime::from_nsec(5500);

// next address in the same half of the RAM (frames relayed from the receive ring wrap inside it)
inline uint16_t next_address(uint16_t address)
{
	return (address & 0x1000) | ((address + 1) & 0x0fff);
}

} // anonymous namespace


// device type definition
DEFINE_DEVICE_TYPE(NAMCO_C139, namco_c139_device, "namco_c139", "Namco C139 Serial")


//**************************************************************************
//  LIVE DEVICE
//**************************************************************************

void namco_c139_device::data_map(address_map &map)
{
	map(0x0000, 0x3fff).rw(FUNC(namco_c139_device::ram_r),FUNC(namco_c139_device::ram_w));
}

void namco_c139_device::regs_map(address_map &map)
{
	map(0x00, 0x0f).rw(FUNC(namco_c139_device::reg_r), FUNC(namco_c139_device::reg_w));
}


//-------------------------------------------------
//  namco_c139_device - constructor
//-------------------------------------------------

namco_c139_device::namco_c139_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, NAMCO_C139, tag, owner, clock),
	m_irq_cb(*this)
{
	auto const &opts = mconfig.options();

	m_localhost = opts.comm_localhost();
	m_localport = opts.comm_localport();
	m_remotehost = opts.comm_remotehost();
	m_remoteport = opts.comm_remoteport();
}


void namco_c139_device::add_local_peer(namco_c139_device &peer)
{
	m_local_peers.push_back(&peer);
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void namco_c139_device::device_start()
{
	m_tick_timer = timer_alloc(FUNC(namco_c139_device::tick_timer_callback), this);
	m_tx_timer = timer_alloc(FUNC(namco_c139_device::tx_done), this);

	m_context = std::make_unique<context>(*this);
	m_context->start();

	// state saving
	save_item(NAME(m_ram));
	save_item(NAME(m_reg));
	save_item(NAME(m_tx_busy));
	save_item(NAME(m_rx_locked));
}


//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void namco_c139_device::device_reset()
{
	std::fill(std::begin(m_ram), std::end(m_ram), 0);
	std::fill(std::begin(m_reg), std::end(m_reg), 0);

	m_tx_busy = false;
	m_rx_locked = false;
	m_rx_irq_time = attotime::zero;
	m_tx_timer->adjust(attotime::never);
	m_local_rx.clear();

	// boards wired together in one machine don't use the network
	if (m_local_peers.empty())
		m_context->reset(m_localhost, m_localport, m_remotehost, m_remoteport);

	m_tick_timer->adjust(attotime::from_hz(2000), 0, attotime::from_hz(2000));
}

void namco_c139_device::device_stop()
{
	m_tick_timer->adjust(attotime::never);
	m_tx_timer->adjust(attotime::never);

	m_context->stop();
	m_context.reset();
}


//**************************************************************************
//  READ/WRITE HANDLERS
//**************************************************************************

uint16_t namco_c139_device::ram_r(offs_t offset)
{
	return m_ram[offset];
}

void namco_c139_device::ram_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	uint16_t value = m_ram[offset];
	COMBINE_DATA(&value);
	value &= 0x1ff;

	// B-9 forces bit 8 of the words written by the host
	if (m_reg[REG_CONTROL] & 1)
		value = (value & 0xff) | ((m_reg[REG_CONTROL] & 2) ? 0x100 : 0);

	m_ram[offset] = value;
}

uint16_t namco_c139_device::reg_r(offs_t offset)
{
	uint16_t result = m_reg[offset];
	if (offset == REG_STATUS)
		result = (result & 3) | (m_tx_busy ? 0 : 4);

	if (!machine().side_effects_disabled())
		LOG("C139: reg_r[%02x] = %04x\n", offset, result);
	return result;
}

void namco_c139_device::reg_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	if (!machine().side_effects_disabled())
		LOG("C139: reg_w[%02x] = %04x\n", offset, data);

	switch (offset)
	{
		case REG_STATUS:
			// clears the interrupt causes written as 0, the transmitter idle bit is live
			m_reg[REG_STATUS] &= (data | ~mem_mask) & 3;
			check_rx();
			break;

		case REG_MODE:
			COMBINE_DATA(&m_reg[REG_MODE]);
			m_reg[REG_MODE] &= 0x0f;
			// clearing the receive mask re-arms the receive interrupt
			if (!(m_reg[REG_MODE] & 2))
				m_rx_locked = false;
			update_irq();
			check_rx();
			break;

		case REG_START:
		case REG_TXSIZE:
			COMBINE_DATA(&m_reg[offset]);
			m_reg[offset] &= 0x1fff;
			check_tx();
			break;

		default:
			COMBINE_DATA(&m_reg[offset]);
			m_reg[offset] &= 0x1fff;
			break;
	}
}


//**************************************************************************
//  LINK
//**************************************************************************

TIMER_CALLBACK_MEMBER(namco_c139_device::tick_timer_callback)
{
	check_tx();
	check_rx();
	update_irq();

	// the receive request stays active until the game handles it: repeat it when it went unanswered,
	// e.g. raised while the interrupt controller level was still disabled (Driver's Eyes side boards
	// at boot, Alpine Racer 2 while it transmits)
	if (m_rx_locked && (m_reg[REG_STATUS] & 2) && (machine().time() - m_rx_irq_time) > attotime::from_msec(1))
	{
		m_rx_irq_time = machine().time();
		m_irq_cb(ASSERT_LINE);
	}
}

void namco_c139_device::check_tx()
{
	if (m_tx_busy || !m_reg[REG_TXSIZE] || (m_reg[REG_START] & 1))
		return;

	// the words are taken now, the game may build its next frame while this one is on the line
	unsigned const count = m_reg[REG_TXSIZE];
	std::vector<uint16_t> words(count);
	uint16_t address = m_reg[REG_TXOFFSET];
	for (unsigned i = 0; i < count; i++)
	{
		words[i] = m_ram[address];
		address = next_address(address);
	}
	m_reg[REG_TXOFFSET] = address;

	LOG("C139: send %d words\n", count);
	if (m_local_peers.empty())
		m_context->send(words);
	else
		for (namco_c139_device *peer : m_local_peers)
			peer->deliver_local(words);

	m_tx_busy = true;
	m_tx_timer->adjust(WORD_TIME * count);
}

TIMER_CALLBACK_MEMBER(namco_c139_device::tx_done)
{
	m_tx_busy = false;
	m_reg[REG_TXSIZE] = 0;
	update_irq();
}

void namco_c139_device::deliver_local(std::vector<uint16_t> const &words)
{
	m_local_rx.push_back(words);
	check_rx();
}

void namco_c139_device::check_rx()
{
	// one frame per receive interrupt: the next one waits until the game has acknowledged the
	// cause and, when an interrupt was raised for it, cleared the receive mask again
	if (m_rx_locked || (m_reg[REG_STATUS] & 2))
		return;

	std::vector<uint16_t> words;
	if (!m_local_rx.empty())
	{
		words = std::move(m_local_rx.front());
		m_local_rx.pop_front();
	}
	else if (!m_local_peers.empty() || !m_context->receive(words))
	{
		return;
	}

	uint16_t address = m_reg[REG_RXOFFSET];
	for (uint16_t const word : words)
	{
		m_ram[0x1000 | (address & 0x0fff)] = word;
		address = next_address(address);
	}
	m_reg[REG_RXOFFSET] = address;
	m_reg[REG_RXSIZE] = (m_reg[REG_RXSIZE] - words.size()) & 0x1fff;
	m_reg[REG_STATUS] |= 2;

	LOG("C139: received %d words\n", words.size());
	update_irq();
}

void namco_c139_device::update_irq()
{
	if (!(m_reg[REG_MODE] & 8))
		return;

	// raising an interrupt masks its cause until the game clears the mask bit
	bool fire = false;
	if ((m_reg[REG_STATUS] & 2) && !(m_reg[REG_MODE] & 2))
	{
		m_reg[REG_MODE] |= 2;
		m_rx_locked = true;
		m_rx_irq_time = machine().time();
		fire = true;
	}
	if (!m_tx_busy && !(m_reg[REG_MODE] & 4))
	{
		m_reg[REG_MODE] |= 4;
		fire = true;
	}

	if (fire)
		m_irq_cb(ASSERT_LINE);
}
