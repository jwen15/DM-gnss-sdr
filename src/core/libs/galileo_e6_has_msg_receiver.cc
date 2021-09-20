/*!
 * \file galileo_e6_has_msg_receiver.cc
 * \brief GNU Radio block that processes Galileo HAS message pages received from
 * Galileo E6B telemetry blocks. After successful decoding, sends the content to
 * the PVT block.
 * \author Carles Fernandez-Prades, 2021. cfernandez(at)cttc.es
 * \author Javier Arribas, 2021. jarribas(at)cttc.es
 *
 * -----------------------------------------------------------------------------
 *
 * GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
 * This file is part of GNSS-SDR.
 *
 * Copyright (C) 2010-2021  (see AUTHORS file for a list of contributors)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */


#include "galileo_e6_has_msg_receiver.h"
#include "display.h"               // for colors in terminal
#include "galileo_has_page.h"      // for Galileo_HAS_page
#include "gnss_sdr_make_unique.h"  // for std::make_unique in C++11
#include "reed_solomon.h"          // for ReedSolomon
#include <boost/any.hpp>
#include <glog/logging.h>
#include <gnuradio/io_signature.h>
#include <algorithm>  // for std::find, std::count
#include <cstddef>    // for size_t
#include <iterator>   // for std::back_inserter
#include <sstream>    // for std::stringstream
#include <stdexcept>  // for std::out_of_range
#include <typeinfo>   // for typeid

#if HAS_GENERIC_LAMBDA
#else
#include <boost/bind/bind.hpp>
#endif


galileo_e6_has_msg_receiver_sptr galileo_e6_has_msg_receiver_make()
{
    return galileo_e6_has_msg_receiver_sptr(new galileo_e6_has_msg_receiver());
}


galileo_e6_has_msg_receiver::galileo_e6_has_msg_receiver() : gr::block("galileo_e6_has_msg_receiver", gr::io_signature::make(0, 0, 0), gr::io_signature::make(0, 0, 0))
{
    // register Gal E6 HAS input message port from telemetry blocks
    this->message_port_register_in(pmt::mp("E6_HAS_from_TLM"));
    // register nav message monitor out
    this->message_port_register_out(pmt::mp("Nav_msg_from_TLM"));
    this->set_msg_handler(pmt::mp("E6_HAS_from_TLM"),
#if HAS_GENERIC_LAMBDA
        [this](auto&& PH1) { msg_handler_galileo_e6_has(PH1); });
#else
#if USE_BOOST_BIND_PLACEHOLDERS
        boost::bind(&galileo_e6_has_msg_receiver::msg_handler_galileo_e6_has, this, boost::placeholders::_1));
#else
        boost::bind(&galileo_e6_has_msg_receiver::msg_handler_galileo_e6_has, this, _1));
#endif
#endif

    // register Gal E6 processed HAS async output message port towards PVT
    this->message_port_register_out(pmt::mp("E6_HAS_to_PVT"));

    // initialize Reed-Solomon decoder
    d_rs = std::make_unique<ReedSolomon>();

    // Reserve memory for decoding matrices and received PIDs
    d_C_matrix = std::vector<std::vector<std::vector<uint8_t>>>(GALILEO_CNAV_INFORMATION_VECTOR_LENGTH, std::vector<std::vector<uint8_t>>(GALILEO_CNAV_MAX_NUMBER_SYMBOLS_ENCODED_BLOCK, std::vector<uint8_t>(GALILEO_CNAV_OCTETS_IN_SUBPAGE)));  // 32 x 255 x 53
    d_M_matrix = std::vector<std::vector<uint8_t>>(GALILEO_CNAV_INFORMATION_VECTOR_LENGTH, std::vector<uint8_t>(GALILEO_CNAV_OCTETS_IN_SUBPAGE));                                                                                                 // HAS message matrix 32 x 53
    d_received_pids = std::vector<std::vector<uint8_t>>(HAS_MSG_NUMBER_MESSAGE_IDS, std::vector<uint8_t>());

    // Reserve memory to store masks
    d_nsat_in_mask_id = std::vector<int>(HAS_MSG_NUMBER_MASK_IDS);
    d_gnss_id_in_mask = std::vector<std::vector<uint8_t>>(HAS_MSG_NUMBER_MASK_IDS, std::vector<uint8_t>(HAS_MSG_NUMBER_GNSS_IDS));
    d_satellite_mask = std::vector<std::vector<uint64_t>>(HAS_MSG_NUMBER_MASK_IDS, std::vector<uint64_t>(HAS_MSG_NUMBER_GNSS_IDS));
    d_signal_mask = std::vector<std::vector<uint16_t>>(HAS_MSG_NUMBER_MASK_IDS, std::vector<uint16_t>(HAS_MSG_NUMBER_GNSS_IDS));
    d_cell_mask_availability_flag = std::vector<std::vector<bool>>(HAS_MSG_NUMBER_MASK_IDS, std::vector<bool>(HAS_MSG_NUMBER_GNSS_IDS));
    d_cell_mask = std::vector<std::vector<std::vector<std::vector<bool>>>>(HAS_MSG_NUMBER_MASK_IDS, {HAS_MSG_NUMBER_GNSS_IDS, {HAS_MSG_NUMBER_SATELLITE_IDS, std::vector<bool>(HAS_MSG_NUMBER_SIGNAL_MASKS)}});
    d_nsys_in_mask = std::vector<uint8_t>(HAS_MSG_NUMBER_MASK_IDS);
    d_nav_message_mask = std::vector<std::vector<uint8_t>>(HAS_MSG_NUMBER_MASK_IDS, std::vector<uint8_t>(HAS_MSG_NUMBER_GNSS_IDS));

    // Initialize values for d_nav_msg_packet
    d_nav_msg_packet.system = std::string("E");
    d_nav_msg_packet.signal = std::string("E6");
    d_nav_msg_packet.prn = 0;
    d_nav_msg_packet.tow_at_current_symbol_ms = 0;
}


void galileo_e6_has_msg_receiver::set_enable_navdata_monitor(bool enable)
{
    d_enable_navdata_monitor = enable;
}


void galileo_e6_has_msg_receiver::msg_handler_galileo_e6_has(const pmt::pmt_t& msg)
{
    gr::thread::scoped_lock lock(d_setlock);  // require mutex with msg_handler_galileo_e6_has function called by the scheduler

    try
        {
            const size_t msg_type_hash_code = pmt::any_ref(msg).type().hash_code();
            if (msg_type_hash_code == typeid(std::shared_ptr<Galileo_HAS_page>).hash_code())
                {
                    const auto HAS_data_page = boost::any_cast<std::shared_ptr<Galileo_HAS_page>>(pmt::any_ref(msg));
                    DLOG(INFO) << "New HAS page received:  "
                               << "Status: " << static_cast<float>(HAS_data_page->has_status) << ", "
                               << "MT: " << static_cast<float>(HAS_data_page->message_type) << ", "
                               << "MID: " << static_cast<float>(HAS_data_page->message_id) << ", "
                               << "MS: " << static_cast<float>(HAS_data_page->message_size) << ", "
                               << "PID: " << static_cast<float>(HAS_data_page->message_page_id);

                    process_HAS_page(*HAS_data_page.get());
                }
            else
                {
                    LOG(WARNING) << "galileo_e6_has_msg_receiver received an unknown object type!";
                }
        }
    catch (const boost::bad_any_cast& e)
        {
            LOG(WARNING) << "galileo_e6_has_msg_receiver Bad any_cast: " << e.what();
        }

    //  Send the resulting decoded HAS data (if available) to PVT
    if (d_new_message == true)
        {
            auto has_data_ptr = std::make_shared<Galileo_HAS_data>(d_HAS_data);
            this->message_port_pub(pmt::mp("E6_HAS_to_PVT"), pmt::make_any(has_data_ptr));
            d_new_message = false;
            DLOG(INFO) << "HAS message sent to the PVT block through the E6_HAS_to_PVT async message port";
        }
}


void galileo_e6_has_msg_receiver::process_HAS_page(const Galileo_HAS_page& has_page)
{
    if (has_page.has_status == 0 || has_page.has_status == 1)
        {
            std::string page_string(has_page.has_message_string);
            if (has_page.message_page_id != 0)  // PID=0 is reserved, ignore it
                {
                    if (has_page.message_type == 1)  // contains satellite corrections
                        {
                            if (has_page.message_id < 32)  // MID range is from 0 to 31
                                {
                                    if (std::find(d_received_pids[has_page.message_id].begin(), d_received_pids[has_page.message_id].end(), has_page.message_page_id) == d_received_pids[has_page.message_id].end())
                                        {
                                            // New pid! Annotate it.
                                            d_received_pids[has_page.message_id].push_back(has_page.message_page_id);
                                            for (int k = 0; k < GALILEO_CNAV_OCTETS_IN_SUBPAGE; k++)
                                                {
                                                    std::string bits8 = page_string.substr(k * 8, 8);
                                                    std::bitset<8> bs(bits8);
                                                    d_C_matrix[has_page.message_id][has_page.message_page_id - 1][k] = static_cast<uint8_t>(bs.to_ulong());
                                                }
                                        }
                                }
                        }
                }
        }

    // If we have received for this message ID a number of pages equal to the message size
    d_new_message = false;
    if (d_received_pids[has_page.message_id].size() == has_page.message_size)
        {
            // Try to decode the message
            int res = decode_message_type1(has_page.message_id, has_page.message_size);

            if (res == 0)
                {
                    // Successful decoding, we have a valid HAS message stored at d_HAS_data
                    std::cout << TEXT_MAGENTA << "New Galileo HAS message type " << static_cast<float>(has_page.message_id)
                              << " received and successfully decoded" << TEXT_RESET << '\n';
                    if (d_nsat_in_mask_id[d_HAS_data.header.mask_id] != 0)
                        {
                            // if we have the mask for that message, it's ready to be sent to PVT
                            d_new_message = true;
                        }
                }
        }
}


int galileo_e6_has_msg_receiver::decode_message_type1(uint8_t message_id, uint8_t message_size)
{
    DLOG(INFO) << "Start decoding of a HAS message";

    // Compute erasure positions
    std::vector<int> erasure_positions;
    erasure_positions.reserve(223);  // Maximum erasure positions ( = number of parity symbols in a block)

    for (uint8_t i = 1; i < message_size + 1; i++)  // we know that from message_size to 32, the value is 0
        {
            if (std::find(d_received_pids[message_id].begin(), d_received_pids[message_id].end(), i) == d_received_pids[message_id].end())
                {
                    erasure_positions.push_back(i - 1);
                }
        }
    for (int i = 33; i < 256; i++)
        {
            if (std::find(d_received_pids[message_id].begin(), d_received_pids[message_id].end(), static_cast<uint8_t>(i)) == d_received_pids[message_id].end())
                {
                    erasure_positions.push_back(i - 1);
                }
        }

    if (erasure_positions.size() > 223)
        {
            // This should not happen! Maybe message_size < PID < 33 ?
            // Don't even try to decode
            std::string msg("Reed Solomon decoding of HAS message is not possible. Received PIDs:");
            std::stringstream ss;
            for (auto pid : d_received_pids[message_id])
                {
                    ss << " " << static_cast<float>(pid);
                }
            ss << ", Message size: " << static_cast<float>(message_size) << "  Message ID: " << static_cast<float>(message_id);
            msg += ss.str();
            LOG(ERROR) << msg;
            d_received_pids[message_id].clear();
            d_C_matrix[message_id] = {GALILEO_CNAV_MAX_NUMBER_SYMBOLS_ENCODED_BLOCK, std::vector<uint8_t>(GALILEO_CNAV_OCTETS_IN_SUBPAGE)};
            return -1;
        }

    DLOG(INFO) << debug_print_vector("List of received PIDs", d_received_pids[message_id]);
    DLOG(INFO) << debug_print_vector("erasure_positions", erasure_positions);
    DLOG(INFO) << debug_print_matrix("C_matrix", d_C_matrix[message_id]);

    // Reset HAS decoded message matrix
    d_M_matrix = {GALILEO_CNAV_INFORMATION_VECTOR_LENGTH, std::vector<uint8_t>(GALILEO_CNAV_OCTETS_IN_SUBPAGE)};

    // Vertical decoding of d_C_matrix
    for (int col = 0; col < GALILEO_CNAV_OCTETS_IN_SUBPAGE; col++)
        {
            std::vector<uint8_t> C_column(GALILEO_CNAV_MAX_NUMBER_SYMBOLS_ENCODED_BLOCK);
            for (auto pid : d_received_pids[message_id])
                {
                    C_column[pid - 1] = d_C_matrix[message_id][pid - 1][col];
                }

            int result = d_rs->decode(C_column, erasure_positions);

            if (result < 0)
                {
                    DLOG(ERROR) << "Decoding of HAS page failed";
                    return -1;
                }

            std::vector<uint8_t> M_column(C_column.begin(), C_column.begin() + GALILEO_CNAV_INFORMATION_VECTOR_LENGTH);
            for (int i = 0; i < GALILEO_CNAV_INFORMATION_VECTOR_LENGTH; i++)
                {
                    d_M_matrix[i][col] = M_column[i];
                }

            DLOG(INFO) << debug_print_vector("C_column entering the decoder", C_column);
            DLOG(INFO) << "Successful HAS page decoding";
        }

    DLOG(INFO) << debug_print_matrix("M_matrix", d_M_matrix);

    // Form the decoded HAS message by reading rows of d_M_matrix
    std::string decoded_message_type_1;
    decoded_message_type_1.reserve(message_size * GALILEO_CNAV_OCTETS_IN_SUBPAGE * 8);
    for (uint8_t row = 0; row < message_size; row++)
        {
            for (int col = 0; col < GALILEO_CNAV_OCTETS_IN_SUBPAGE; col++)
                {
                    std::bitset<8> bs(d_M_matrix[row][col]);
                    decoded_message_type_1 += bs.to_string();
                }
        }

    DLOG(INFO) << "Decoded message ID " << static_cast<float>(message_id)
               << " (size: " << static_cast<float>(message_size) << ") with header:\n"
               << std::string(decoded_message_type_1.begin(), decoded_message_type_1.begin() + GALILEO_CNAV_MT1_HEADER_BITS)
               << "\nand body:\n"
               << std::string(decoded_message_type_1.begin() + GALILEO_CNAV_MT1_HEADER_BITS, decoded_message_type_1.end());

    if (d_enable_navdata_monitor)
        {
            d_nav_msg_packet.nav_message = decoded_message_type_1;
            const std::shared_ptr<Nav_Message_Packet> tmp_obj = std::make_shared<Nav_Message_Packet>(d_nav_msg_packet);
            this->message_port_pub(pmt::mp("Nav_msg_from_TLM"), pmt::make_any(tmp_obj));
        }

    // reset data for next decoding
    d_C_matrix[message_id] = std::vector<std::vector<uint8_t>>(GALILEO_CNAV_MAX_NUMBER_SYMBOLS_ENCODED_BLOCK, std::vector<uint8_t>(GALILEO_CNAV_OCTETS_IN_SUBPAGE));
    d_received_pids[message_id].clear();

    // Trigger HAS message content reading and fill the d_HAS_data object
    d_HAS_data = Galileo_HAS_data();

    read_MT1_header(decoded_message_type_1.substr(0, GALILEO_CNAV_MT1_HEADER_BITS));

    try
        {
            read_MT1_body(std::string(decoded_message_type_1.begin() + GALILEO_CNAV_MT1_HEADER_BITS, decoded_message_type_1.end()));
        }
    catch (const std::out_of_range& oor)
        {
            std::cerr << "Error when reading decoded HAS data. Wrong data formatting? The error was: " << oor.what() << '\n';
            return -1;
        }
    catch (const std::bad_alloc& e)
        {
            std::cerr << "Error when reading decoded HAS data. Wrong data formatting? The error was: " << e.what() << '\n';
            return -1;
        }
    catch (const std::exception& e)
        {
            std::cerr << "Error when reading decoded HAS data. Wrong data formatting? The error was: " << e.what() << '\n';
            return -1;
        }
    catch (...)
        {
            std::cerr << "Error when reading decoded HAS data. Wrong data formatting?\n";
            return -1;
        }
    return 0;
}


void galileo_e6_has_msg_receiver::read_MT1_header(const std::string& message_header)
{
    // ICD v1.2 Table 6: MT1 Message Header
    const std::bitset<GALILEO_CNAV_MT1_HEADER_BITS> has_mt1_header(message_header);
    d_HAS_data.header.toh = read_has_message_header_parameter_uint16(has_mt1_header, GALILEO_MT1_HEADER_TOH);
    d_HAS_data.header.mask_id = read_has_message_header_parameter_uint8(has_mt1_header, GALILEO_MT1_HEADER_MASK_ID);
    d_HAS_data.header.iod_id = read_has_message_header_parameter_uint8(has_mt1_header, GALILEO_MT1_HEADER_IOD_ID);
    d_HAS_data.header.mask_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_MASK_FLAG);
    d_HAS_data.header.orbit_correction_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_ORBIT_CORRECTION_FLAG);
    d_HAS_data.header.clock_fullset_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_CLOCK_FULLSET_FLAG);
    d_HAS_data.header.clock_subset_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_CLOCK_SUBSET_FLAG);
    d_HAS_data.header.code_bias_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_CODE_BIAS_FLAG);
    d_HAS_data.header.phase_bias_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_PHASE_BIAS_FLAG);
    d_HAS_data.header.ura_flag = read_has_message_header_parameter_bool(has_mt1_header, GALILEO_MT1_HEADER_URA_FLAG);

    DLOG(INFO) << "MT1 header " << message_header << ":  "
               << "TOH: " << static_cast<float>(d_HAS_data.header.toh) << ", "
               << "mask ID: " << static_cast<float>(d_HAS_data.header.mask_id) << ", "
               << "iod ID: " << static_cast<float>(d_HAS_data.header.iod_id) << ", "
               << "mask_flag: " << static_cast<float>(d_HAS_data.header.mask_flag) << ", "
               << "orbit_correction_flag: " << static_cast<float>(d_HAS_data.header.orbit_correction_flag) << ", "
               << "clock_fullset_flag: " << static_cast<float>(d_HAS_data.header.clock_fullset_flag) << ", "
               << "clock_subset_flag: " << static_cast<float>(d_HAS_data.header.clock_subset_flag) << ", "
               << "code_bias_flag: " << static_cast<float>(d_HAS_data.header.code_bias_flag) << ", "
               << "phase_bias_flag: " << static_cast<float>(d_HAS_data.header.phase_bias_flag) << ", "
               << "ura_flag: " << static_cast<float>(d_HAS_data.header.ura_flag);
}


void galileo_e6_has_msg_receiver::read_MT1_body(const std::string& message_body)
{
    // ICD v1.2 Table 7: MT1 Message Body.
    auto message = std::string(message_body);
    int Nsat = 0;
    bool have_mask = false;

    if (d_HAS_data.header.mask_flag)
        {
            // read mask
            d_HAS_data.Nsys = read_has_message_body_uint8(message.substr(0, HAS_MSG_NSYS_LENGTH));
            d_nsys_in_mask[d_HAS_data.header.mask_id] = d_HAS_data.Nsys;
            if (d_HAS_data.Nsys != 0)
                {
                    message = std::string(message.begin() + HAS_MSG_NSYS_LENGTH, message.end());
                    d_HAS_data.gnss_id_mask = std::vector<uint8_t>(d_HAS_data.Nsys);
                    d_HAS_data.cell_mask = {d_HAS_data.Nsys, std::vector<std::vector<bool>>(40, std::vector<bool>(16))};
                    d_HAS_data.cell_mask_availability_flag = std::vector<bool>(d_HAS_data.Nsys);
                    d_HAS_data.nav_message = std::vector<uint8_t>(d_HAS_data.Nsys);
                    d_HAS_data.satellite_mask = std::vector<uint64_t>(d_HAS_data.Nsys);
                    d_HAS_data.signal_mask = std::vector<uint16_t>(d_HAS_data.Nsys);

                    for (uint8_t i = 0; i < d_HAS_data.Nsys; i++)
                        {
                            d_HAS_data.gnss_id_mask[i] = read_has_message_body_uint8(message.substr(0, HAS_MSG_ID_MASK_LENGTH));
                            d_gnss_id_in_mask[d_HAS_data.header.mask_id][i] = d_HAS_data.gnss_id_mask[i];
                            message = std::string(message.begin() + HAS_MSG_ID_MASK_LENGTH, message.end());

                            std::string msg = message.substr(0, HAS_MSG_SATELLITE_MASK_LENGTH);
                            d_HAS_data.satellite_mask[i] = read_has_message_body_uint64(msg);
                            d_satellite_mask[d_HAS_data.header.mask_id][i] = d_HAS_data.satellite_mask[i];
                            int ones_in_satellite_mask = std::count(msg.begin(), msg.end(), '1');
                            Nsat += ones_in_satellite_mask;
                            message = std::string(message.begin() + HAS_MSG_SATELLITE_MASK_LENGTH, message.end());

                            msg = message.substr(0, HAS_MSG_SIGNAL_MASK_LENGTH);
                            d_HAS_data.signal_mask[i] = read_has_message_body_uint16(msg);
                            d_signal_mask[d_HAS_data.header.mask_id][i] = d_HAS_data.signal_mask[i];
                            int ones_in_signal_mask = std::count(msg.begin(), msg.end(), '1');
                            message = std::string(message.begin() + HAS_MSG_SIGNAL_MASK_LENGTH, message.end());

                            d_HAS_data.cell_mask[i] = std::vector<std::vector<bool>>(ones_in_satellite_mask, std::vector<bool>(ones_in_signal_mask, false));

                            if (message.substr(0, 1) == "1")
                                {
                                    d_HAS_data.cell_mask_availability_flag[i] = true;
                                }
                            else
                                {
                                    d_HAS_data.cell_mask_availability_flag[i] = false;
                                }
                            d_cell_mask_availability_flag[d_HAS_data.header.mask_id][i] = d_HAS_data.cell_mask_availability_flag[i];
                            message = std::string(message.begin() + 1, message.end());

                            int size_cell = ones_in_satellite_mask * ones_in_signal_mask;
                            int pos = 0;
                            for (int s = 0; s < ones_in_satellite_mask; s++)
                                {
                                    for (int sig = 0; sig < ones_in_signal_mask; sig++)
                                        {
                                            d_HAS_data.cell_mask[i][s][sig] = (message[pos] == '1' ? true : false);
                                            pos++;
                                        }
                                }
                            message = std::string(message.begin() + size_cell, message.end());

                            d_HAS_data.nav_message[i] = read_has_message_body_uint8(message.substr(0, HAS_MSG_NAV_MESSAGE_LENGTH));
                            d_nav_message_mask[d_HAS_data.header.mask_id][i] = d_HAS_data.nav_message[i];
                            message = std::string(message.begin() + HAS_MSG_NAV_MESSAGE_LENGTH, message.end());
                        }
                    d_nsat_in_mask_id[d_HAS_data.header.mask_id] = Nsat;
                    d_cell_mask[d_HAS_data.header.mask_id] = d_HAS_data.cell_mask;
                    message = std::string(message.begin() + HAS_MSG_MASK_RESERVED_LENGTH, message.end());
                }
            if (Nsat != 0)
                {
                    have_mask = true;
                }

            DLOG(INFO) << "Nsys: " << static_cast<float>(d_HAS_data.Nsys);
            DLOG(INFO) << debug_print_vector("GNSS ID", d_HAS_data.gnss_id_mask);
            DLOG(INFO) << debug_print_vector("cell_mask_availability_flag", d_HAS_data.cell_mask_availability_flag);
            // for (uint8_t k = 0; k < d_HAS_data.Nsys; k++)
            //     {
            //         std::string aux = "cell_mask " + std::to_string(k);
            //         DLOG(INFO) << debug_print_matrix(aux, d_HAS_data.cell_mask[k]);
            //     }
            DLOG(INFO) << debug_print_vector("nav_message", d_HAS_data.nav_message);
        }
    else
        {
            // Take data from a previously received mask with the same mask_id
            Nsat = d_nsat_in_mask_id[d_HAS_data.header.mask_id];
            if (Nsat != 0)
                {
                    d_HAS_data.Nsys = d_nsys_in_mask[d_HAS_data.header.mask_id];
                    std::copy(d_gnss_id_in_mask[d_HAS_data.header.mask_id].begin(), d_gnss_id_in_mask[d_HAS_data.header.mask_id].begin() + d_HAS_data.Nsys, std::back_inserter(d_HAS_data.gnss_id_mask));
                    std::copy(d_satellite_mask[d_HAS_data.header.mask_id].begin(), d_satellite_mask[d_HAS_data.header.mask_id].begin() + d_HAS_data.Nsys, std::back_inserter(d_HAS_data.satellite_mask));
                    std::copy(d_signal_mask[d_HAS_data.header.mask_id].begin(), d_signal_mask[d_HAS_data.header.mask_id].begin() + d_HAS_data.Nsys, std::back_inserter(d_HAS_data.signal_mask));
                    std::copy(d_cell_mask_availability_flag[d_HAS_data.header.mask_id].begin(), d_cell_mask_availability_flag[d_HAS_data.header.mask_id].begin() + d_HAS_data.Nsys, std::back_inserter(d_HAS_data.cell_mask_availability_flag));
                    d_HAS_data.cell_mask = d_cell_mask[d_HAS_data.header.mask_id];
                    std::copy(d_nav_message_mask[d_HAS_data.header.mask_id].begin(), d_nav_message_mask[d_HAS_data.header.mask_id].begin() + d_HAS_data.Nsys, std::back_inserter(d_HAS_data.nav_message));

                    have_mask = true;

                    DLOG(INFO) << "Nsys: " << static_cast<float>(d_HAS_data.Nsys);
                    DLOG(INFO) << debug_print_vector("GNSS ID", d_HAS_data.gnss_id_mask);
                    DLOG(INFO) << debug_print_vector("cell_mask_availability_flag", d_HAS_data.cell_mask_availability_flag);
                    // for (uint8_t k = 0; k < d_HAS_data.Nsys; k++)
                    //     {
                    //         std::string aux = "cell_mask " + std::to_string(k);
                    //         DLOG(INFO) << debug_print_matrix(aux, d_HAS_data.cell_mask[k]);
                    //     }
                    DLOG(INFO) << debug_print_vector("nav_message", d_HAS_data.nav_message);
                }
        }

    // discard data if crazy Values
    if (d_HAS_data.header.toh > HAS_MSG_NUMBER_MAX_TOH)
        {
            have_mask = false;
            d_nsat_in_mask_id[d_HAS_data.header.mask_id] = 0;
        }

    if (d_HAS_data.header.orbit_correction_flag && have_mask)
        {
            // read orbit corrections
            d_HAS_data.validity_interval_index_orbit_corrections = read_has_message_body_uint8(message.substr(0, HAS_MSG_VALIDITY_INDEX_LENGTH));
            message = std::string(message.begin() + HAS_MSG_VALIDITY_INDEX_LENGTH, message.end());
            d_HAS_data.gnss_iod = std::vector<uint16_t>(Nsat);
            d_HAS_data.delta_radial = std::vector<int16_t>(Nsat);
            d_HAS_data.delta_along_track = std::vector<int16_t>(Nsat);
            d_HAS_data.delta_cross_track = std::vector<int16_t>(Nsat);
            for (int i = 0; i < Nsat; i++)
                {
                    if (d_HAS_data.gnss_id_mask[i] == HAS_MSG_GPS_SYSTEM)
                        {
                            d_HAS_data.gnss_iod[i] = read_has_message_body_uint16(message.substr(0, HAS_MSG_IOD_GPS_LENGTH));
                            message = std::string(message.begin() + HAS_MSG_IOD_GPS_LENGTH, message.end());
                        }
                    if (d_HAS_data.gnss_id_mask[i] == HAS_MSG_GALILEO_SYSTEM)
                        {
                            d_HAS_data.gnss_iod[i] = read_has_message_body_uint16(message.substr(0, HAS_MSG_IOD_GAL_LENGTH));
                            message = std::string(message.begin() + HAS_MSG_IOD_GAL_LENGTH, message.end());
                        }
                    d_HAS_data.delta_radial[i] = read_has_message_body_int16(message.substr(0, HAS_MSG_DELTA_RADIAL_LENGTH));
                    message = std::string(message.begin() + HAS_MSG_DELTA_RADIAL_LENGTH, message.end());

                    d_HAS_data.delta_along_track[i] = read_has_message_body_int16(message.substr(0, HAS_MSG_DELTA_ALONG_TRACK_LENGTH));
                    message = std::string(message.begin() + HAS_MSG_DELTA_ALONG_TRACK_LENGTH, message.end());

                    d_HAS_data.delta_cross_track[i] = read_has_message_body_int16(message.substr(0, HAS_MSG_DELTA_CROSS_TRACK_LENGTH));
                    message = std::string(message.begin() + HAS_MSG_DELTA_CROSS_TRACK_LENGTH, message.end());
                }

            DLOG(INFO) << debug_print_vector("gnss_iod", d_HAS_data.gnss_iod);
            DLOG(INFO) << debug_print_vector("delta_radial", d_HAS_data.delta_radial);
            DLOG(INFO) << debug_print_vector("delta_along_track", d_HAS_data.delta_along_track);
            DLOG(INFO) << debug_print_vector("delta_cross_track", d_HAS_data.delta_cross_track);
        }

    if (d_HAS_data.header.clock_fullset_flag && have_mask)
        {
            // read clock full-set corrections
            d_HAS_data.validity_interval_index_clock_fullset_corrections = read_has_message_body_uint8(message.substr(0, HAS_MSG_VALIDITY_INDEX_LENGTH));
            message = std::string(message.begin() + HAS_MSG_VALIDITY_INDEX_LENGTH, message.end());

            d_HAS_data.delta_clock_c0_multiplier = std::vector<uint8_t>(d_HAS_data.Nsys);
            for (uint8_t i = 0; i < d_HAS_data.Nsys; i++)
                {
                    d_HAS_data.delta_clock_c0_multiplier[i] = read_has_message_body_uint8(message.substr(0, HAS_MSG_DELTA_CLOCK_C0_MULTIPLIER_LENGTH));
                    message = std::string(message.begin() + HAS_MSG_DELTA_CLOCK_C0_MULTIPLIER_LENGTH, message.end());
                }

            d_HAS_data.iod_change_flag = std::vector<bool>(Nsat);
            d_HAS_data.delta_clock_c0 = std::vector<int16_t>(Nsat);
            for (int i = 0; i < Nsat; i++)
                {
                    d_HAS_data.iod_change_flag[i] = (message[0] == '1' ? true : false);
                    message = std::string(message.begin() + 1, message.end());
                    d_HAS_data.delta_clock_c0[i] = read_has_message_body_int16(message.substr(0, HAS_MSG_DELTA_CLOCK_C0_LENGTH));
                    message = std::string(message.begin() + HAS_MSG_DELTA_CLOCK_C0_LENGTH, message.end());
                }

            DLOG(INFO) << debug_print_vector("delta_clock_c0_multiplier", d_HAS_data.delta_clock_c0_multiplier);
            DLOG(INFO) << debug_print_vector("delta_clock_c0", d_HAS_data.delta_clock_c0);
        }

    if (d_HAS_data.header.clock_subset_flag && have_mask)
        {
            // read clock subset corrections
            d_HAS_data.validity_interval_index_clock_subset_corrections = read_has_message_body_uint8(message.substr(0, HAS_MSG_VALIDITY_INDEX_LENGTH));
            message = std::string(message.begin() + HAS_MSG_VALIDITY_INDEX_LENGTH, message.end());

            d_HAS_data.Nsysprime = read_has_message_body_uint8(message.substr(0, HAS_MSG_NSYSPRIME_LENGTH));
            message = std::string(message.begin() + HAS_MSG_NSYSPRIME_LENGTH, message.end());

            if (d_HAS_data.Nsysprime == 0)
                {
                    // Wrong formatted data, aborting
                    have_mask = false;
                    d_nsat_in_mask_id[d_HAS_data.header.mask_id] = 0;
                }

            d_HAS_data.gnss_id_clock_subset = std::vector<uint8_t>(d_HAS_data.Nsysprime);
            d_HAS_data.delta_clock_c0_multiplier_clock_subset = std::vector<uint8_t>(d_HAS_data.Nsysprime);
            d_HAS_data.satellite_submask = std::vector<std::vector<uint64_t>>(d_HAS_data.Nsysprime, std::vector<uint64_t>(HAS_MSG_NUMBER_SATELLITE_IDS));
            d_HAS_data.iod_change_flag_clock_subset = std::vector<bool>(d_HAS_data.Nsysprime);
            d_HAS_data.delta_clock_c0_clock_subset = std::vector<int16_t>(d_HAS_data.Nsysprime);
            for (uint8_t i = 0; i < d_HAS_data.Nsysprime; i++)
                {
                    d_HAS_data.gnss_id_clock_subset[i] = read_has_message_body_uint8(message.substr(0, HAS_MSG_ID_CLOCK_SUBSET_LENGTH));
                    message = std::string(message.begin() + HAS_MSG_ID_CLOCK_SUBSET_LENGTH, message.end());

                    uint8_t clock_multiplier = read_has_message_body_uint8(message.substr(0, HAS_MSG_DELTA_CLOCK_MULTIPLIER_SUBSET_LENGTH));
                    d_HAS_data.delta_clock_c0_multiplier_clock_subset[i] = clock_multiplier + 1;
                    message = std::string(message.begin() + HAS_MSG_DELTA_CLOCK_MULTIPLIER_SUBSET_LENGTH, message.end());

                    uint64_t satellite_mask = d_HAS_data.satellite_mask[i];
                    std::bitset<40> satellite_mask_bits(satellite_mask);
                    std::string satellite_mask_string = satellite_mask_bits.to_string();
                    int number_sats_this_gnss_id = std::count(satellite_mask_string.begin(), satellite_mask_string.end(), '1');
                    d_HAS_data.satellite_submask[i] = std::vector<uint64_t>(number_sats_this_gnss_id);
                    for (int j = 0; j < number_sats_this_gnss_id; j++)
                        {
                            d_HAS_data.satellite_submask[i][j] = read_has_message_body_uint64(message.substr(0, 1));
                            message = std::string(message.begin() + 1, message.end());
                        }

                    int Nsatprime = std::count(d_HAS_data.satellite_submask[i].begin(), d_HAS_data.satellite_submask[i].end(), 1UL);
                    for (int j = 0; j < Nsatprime; j++)
                        {
                            d_HAS_data.delta_clock_c0_clock_subset[i] = read_has_message_body_int16(message.substr(0, HAS_MSG_DELTA_CLOCK_C0_SUBSET_LENGTH));
                            message = std::string(message.begin() + HAS_MSG_DELTA_CLOCK_C0_SUBSET_LENGTH, message.end());
                        }
                }

            DLOG(INFO) << "Nsysprime: " << static_cast<float>(d_HAS_data.Nsysprime);
            DLOG(INFO) << (d_HAS_data.Nsysprime == 0 ? "" : debug_print_vector("gnss_id_clock_subset", d_HAS_data.gnss_id_clock_subset));
            DLOG(INFO) << (d_HAS_data.Nsysprime == 0 ? "" : debug_print_vector("delta_clock_c0_multiplier_clock_subset", d_HAS_data.delta_clock_c0_multiplier_clock_subset));
            DLOG(INFO) << (d_HAS_data.Nsysprime == 0 ? "" : debug_print_matrix("satellite_submask", d_HAS_data.satellite_submask));
            DLOG(INFO) << (d_HAS_data.Nsysprime == 0 ? "" : debug_print_vector("delta_clock_c0_clock_subset", d_HAS_data.delta_clock_c0_clock_subset));
        }

    if (d_HAS_data.header.code_bias_flag && have_mask)
        {
            // read code bias
            d_HAS_data.validity_interval_index_code_bias_corrections = read_has_message_body_uint8(message.substr(0, HAS_MSG_VALIDITY_INDEX_LENGTH));
            message = std::string(message.begin() + HAS_MSG_VALIDITY_INDEX_LENGTH, message.end());

            std::vector<uint64_t> number_sats(d_HAS_data.Nsys);
            std::vector<uint64_t> number_codes(d_HAS_data.Nsys);
            uint64_t max_signals = 0ULL;
            for (int sys = 0; sys < d_HAS_data.Nsys; sys++)
                {
                    uint64_t number_sats_this_gnss_id = 0;
                    uint64_t number_signals_this_gnss_id = 0;
                    if (d_HAS_data.cell_mask_availability_flag[sys] == true)
                        {
                            // cell mask is provided
                            number_sats_this_gnss_id = d_HAS_data.cell_mask[sys].size();
                            number_signals_this_gnss_id = d_HAS_data.cell_mask[sys][0].size();

                            if (number_signals_this_gnss_id > max_signals)
                                {
                                    max_signals = number_signals_this_gnss_id;
                                }
                        }
                    else
                        {
                            // corrections for all satellites in satellite_mask
                            // and all signals in signal mask
                            uint64_t n = d_HAS_data.satellite_mask[sys];
                            while (n)
                                {
                                    number_sats_this_gnss_id += n & 1;
                                    n >>= 1;
                                }
                            uint64_t m = d_HAS_data.signal_mask[sys];
                            while (m)
                                {
                                    number_signals_this_gnss_id += m & 1;
                                    m >>= 1;
                                }
                            if (number_signals_this_gnss_id > max_signals)
                                {
                                    max_signals = number_signals_this_gnss_id;
                                }
                        }
                    number_sats[sys] = number_sats_this_gnss_id;
                    number_codes[sys] = number_signals_this_gnss_id;
                }

            d_HAS_data.code_bias = std::vector<std::vector<int16_t>>(Nsat, std::vector<int16_t>(max_signals));

            int sat = 0;
            for (int sys = 0; sys < d_HAS_data.Nsys; sys++)
                {
                    for (uint64_t s = 0; s < number_sats[sys]; s++)
                        {
                            for (uint64_t c = 0; c < number_codes[sys]; c++)
                                {
                                    if ((d_HAS_data.cell_mask_availability_flag[sys] == false) || ((d_HAS_data.cell_mask_availability_flag[sys] == true) && (d_HAS_data.cell_mask[sys][s][c])))
                                        {
                                            d_HAS_data.code_bias[sat][c] = read_has_message_body_int16(message.substr(0, HAS_MSG_CODE_BIAS_LENGTH));
                                            message = std::string(message.begin() + HAS_MSG_CODE_BIAS_LENGTH, message.end());
                                        }
                                }
                            sat += 1;
                        }
                }

            DLOG(INFO) << debug_print_matrix("code bias", d_HAS_data.code_bias);
        }

    if (d_HAS_data.header.phase_bias_flag && have_mask)
        {
            // read phase bias
            d_HAS_data.validity_interval_index_phase_bias_corrections = read_has_message_body_uint8(message.substr(0, HAS_MSG_VALIDITY_INDEX_LENGTH));
            message = std::string(message.begin() + HAS_MSG_VALIDITY_INDEX_LENGTH, message.end());

            std::vector<uint64_t> number_sats(d_HAS_data.Nsys);
            std::vector<uint64_t> number_phases(d_HAS_data.Nsys);
            uint64_t max_signals = 0ULL;
            for (int sys = 0; sys < d_HAS_data.Nsys; sys++)
                {
                    uint64_t number_sats_this_gnss_id = 0;
                    uint64_t number_signals_this_gnss_id = 0;
                    if (d_HAS_data.cell_mask_availability_flag[sys] == true)
                        {
                            // cell mask is provided
                            number_sats_this_gnss_id = d_HAS_data.cell_mask[sys].size();
                            number_signals_this_gnss_id = d_HAS_data.cell_mask[sys][0].size();

                            if (number_signals_this_gnss_id > max_signals)
                                {
                                    max_signals = number_signals_this_gnss_id;
                                }
                        }
                    else
                        {
                            // corrections for all satellites in satellite_mask
                            // and all signals in signal mask
                            uint64_t n = d_HAS_data.satellite_mask[sys];
                            while (n)
                                {
                                    number_sats_this_gnss_id += n & 1;
                                    n >>= 1;
                                }
                            uint64_t m = d_HAS_data.signal_mask[sys];
                            while (m)
                                {
                                    number_signals_this_gnss_id += m & 1;
                                    m >>= 1;
                                }
                            if (number_signals_this_gnss_id > max_signals)
                                {
                                    max_signals = number_signals_this_gnss_id;
                                }
                        }
                    number_sats[sys] = number_sats_this_gnss_id;
                    number_phases[sys] = number_signals_this_gnss_id;
                }

            d_HAS_data.phase_bias = std::vector<std::vector<int16_t>>(Nsat, std::vector<int16_t>(max_signals));
            d_HAS_data.phase_discontinuity_indicator = std::vector<std::vector<uint8_t>>(Nsat, std::vector<uint8_t>(max_signals));

            int sat = 0;
            for (int sys = 0; sys < d_HAS_data.Nsys; sys++)
                {
                    for (uint64_t s = 0; s < number_sats[sys]; s++)
                        {
                            for (uint64_t p = 0; p < number_phases[sys]; p++)
                                {
                                    if ((d_HAS_data.cell_mask_availability_flag[sys] == false) || ((d_HAS_data.cell_mask_availability_flag[sys] == true) && (d_HAS_data.cell_mask[sys][s][p])))
                                        {
                                            d_HAS_data.phase_bias[sat][p] = read_has_message_body_int16(message.substr(0, HAS_MSG_PHASE_BIAS_LENGTH));
                                            message = std::string(message.begin() + HAS_MSG_PHASE_BIAS_LENGTH, message.end());

                                            d_HAS_data.phase_discontinuity_indicator[sat][p] = read_has_message_body_uint8(message.substr(0, HAS_MSG_PHASE_DISCONTINUITY_INDICATOR_LENGTH));
                                            message = std::string(message.begin() + HAS_MSG_PHASE_DISCONTINUITY_INDICATOR_LENGTH, message.end());
                                        }
                                }
                            sat += 1;
                        }
                }

            DLOG(INFO) << debug_print_matrix("phase bias", d_HAS_data.phase_bias);
            DLOG(INFO) << debug_print_matrix("phase discontinuity indicator", d_HAS_data.phase_discontinuity_indicator);
        }

    // if (d_HAS_data.header.ura_flag && have_mask)
    //     {
    //         // read URA
    //         d_HAS_data.validity_interval_index_ura_corrections = read_has_message_body_uint8(message.substr(0, HAS_MSG_VALIDITY_INDEX_LENGTH));
    //         message = std::string(message.begin() + HAS_MSG_VALIDITY_INDEX_LENGTH, message.end());
    //         d_HAS_data.ura.reserve(Nsat);
    //         for (int i = 0; i < Nsat; i++)
    //             {
    //                 d_HAS_data.ura[i] = read_has_message_body_uint8(message.substr(0, HAS_MSG_URA_LENGTH));
    //                 message = std::string(message.begin() + HAS_MSG_URA_LENGTH, message.end());
    //             }
    //     }
}


uint16_t galileo_e6_has_msg_receiver::read_has_message_header_parameter_uint16(const std::bitset<GALILEO_CNAV_MT1_HEADER_BITS>& bits, const std::pair<int32_t, int32_t>& parameter) const
{
    uint16_t value = 0U;
    for (int j = 0; j < parameter.second; j++)
        {
            value <<= 1U;  // shift left
            if (static_cast<int>(bits[GALILEO_CNAV_MT1_HEADER_BITS - parameter.first - j]) == 1)
                {
                    value += 1;  // insert the bit
                }
        }
    return value;
}


uint8_t galileo_e6_has_msg_receiver::read_has_message_header_parameter_uint8(const std::bitset<GALILEO_CNAV_MT1_HEADER_BITS>& bits, const std::pair<int32_t, int32_t>& parameter) const
{
    uint8_t value = 0U;
    for (int j = 0; j < parameter.second; j++)
        {
            value <<= 1U;  // shift left
            if (static_cast<int>(bits[GALILEO_CNAV_MT1_HEADER_BITS - parameter.first - j]) == 1)
                {
                    value += 1;  // insert the bit
                }
        }
    return value;
}


bool galileo_e6_has_msg_receiver::read_has_message_header_parameter_bool(const std::bitset<GALILEO_CNAV_MT1_HEADER_BITS>& bits, const std::pair<int32_t, int32_t>& parameter) const
{
    bool value = false;
    if (static_cast<int>(bits[GALILEO_CNAV_MT1_HEADER_BITS - parameter.first]) == 1)
        {
            value = true;
        }
    return value;
}


uint64_t galileo_e6_has_msg_receiver::read_has_message_body_uint64(const std::string& bits) const
{
    uint64_t value = 0U;
    const size_t len = bits.length();

    for (size_t j = 0; j < len; j++)
        {
            value <<= 1U;  // shift left
            if (bits[j] == '1')
                {
                    value += 1;  // insert the bit
                }
        }
    return value;
}


uint16_t galileo_e6_has_msg_receiver::read_has_message_body_uint16(const std::string& bits) const
{
    uint16_t value = 0U;
    const size_t len = bits.length();

    for (size_t j = 0; j < len; j++)
        {
            value <<= 1U;  // shift left
            if (bits[j] == '1')
                {
                    value += 1;  // insert the bit
                }
        }
    return value;
}


int16_t galileo_e6_has_msg_receiver::read_has_message_body_int16(const std::string& bits) const
{
    int16_t value = 0;
    const size_t len = bits.length();

    // read the MSB and perform the sign extension
    if (bits[0] == '1')
        {
            value ^= 0xFFFF;  // 16 bits variable
        }
    else
        {
            value &= 0;
        }

    for (size_t j = 0; j < len; j++)
        {
            value *= 2;       // shift left the signed integer
            value &= 0xFFFE;  // reset the corresponding bit (for the 16 bits variable)
            if (bits[j] == '1')
                {
                    value += 1;  // insert the bit
                }
        }

    return value;
}


uint8_t galileo_e6_has_msg_receiver::read_has_message_body_uint8(const std::string& bits) const
{
    uint8_t value = 0U;
    const size_t len = bits.length();
    for (size_t j = 0; j < len; j++)
        {
            value <<= 1U;  // shift left
            if (bits[j] == '1')
                {
                    value += 1;  // insert the bit
                }
        }
    return value;
}


template <class T>
std::string galileo_e6_has_msg_receiver::debug_print_vector(const std::string& title, const std::vector<T>& vec) const
{
    std::string msg(title);
    msg += ": \n";
    std::stringstream ss;
    for (auto el : vec)
        {
            ss << static_cast<float>(el) << " ";
        }
    msg += ss.str();
    return msg;
}


template <class T>
std::string galileo_e6_has_msg_receiver::debug_print_matrix(const std::string& title, const std::vector<std::vector<T>>& mat) const
{
    std::string msg(title);
    msg += ": \n";
    std::stringstream ss;

    if (!mat.empty())
        {
            for (size_t row = 0; row < mat.size(); row++)
                {
                    for (size_t col = 0; col < mat[0].size(); col++)
                        {
                            ss << static_cast<float>(mat[row][col]) << " ";
                        }
                    ss << '\n';
                }
        }
    else
        {
            ss << '\n';
        }
    msg += ss.str();
    return msg;
}
