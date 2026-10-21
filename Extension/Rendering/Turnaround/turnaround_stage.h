#pragma once
#include "turnaround_session.h"
#include <memory>

// The game side of a turnaround: the Board screen's board, turned and shown over a dark or a light
// background (turnaround_stage.cpp).
namespace dingosdk::turnaround {
std::shared_ptr<Stage> board_stage(const Options& options);
} // namespace dingosdk::turnaround
