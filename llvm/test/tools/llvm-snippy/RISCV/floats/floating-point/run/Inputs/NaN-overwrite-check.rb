#!/usr/bin/env ruby

require 'optparse'

# This program is used to check whether the float registers on the spike model
# that have become NaN boxed are overwritten when the set value is reached. 

# run: ruby NaN-overwrite-check.rb <spike-trace> <overwrite-value> --mattrs +d,+f... \
#          --instructions fdiv.s,fmul.h...

# This method parses the instruction name, register name, and register value
# from two lines of input.
def parse_instruction_lines(line1, line2)

  result = {}
  # Parse the instruction name from the first line, e.g.:
  # core   0: 0x0000000000210090 (0x196309d3) fdiv.s  fs3, ft6, fs6
  match1 = line1.match(%r{core\s+\d+:\s+0x[0-9a-f]+\s+\(0x[0-9a-f]+\)
                          \s+([a-zA-Z0-9.]+)\s+([a-zA-Z0-9]+)}x)
  unless match1
    puts "Error: Failed to parse instruction name from line 1: #{line1}"
    return nil
  end
  result[:instruction_name] = match1[1]
  result[:register_name] = match1[2]

  # Parse the register value from the second line, e.g.:
  # core   0: 3 0x0000000000213744 (0xf40502d3) f5  0xffffffffffff0000
  match2 = line2.match(%r{core\s+\d+:\s+\d+\s+0x[0-9a-f]+\s+\(0x[0-9a-f]+\)
                                  \s+([a-zA-Z0-9_]+)\s+(0x[0-9a-f]+)}x)
  if match2
    result[:register_value] = match2[2]
    return result
  end

  puts "Error: Failed to parse register value from line 2: #{line2}"
  return nil
end

def process_file(filename, nan_ratio, target_instructions, mattrs)
  state = {
    parsing_started: false,
    previous_line: nil,
    registers: {},
    register_count: 0,
    nan_ratio_reached: false,
    fmv_count: 0,
    found_fmv_count: 0,
    success: true
  }

  fmv_instructions = ["fmv.d.x", "fmv.s.x", "fmv.w.x"].freeze

  File.foreach(filename) do |line|
    process_line(state, line.chomp, nan_ratio, target_instructions, mattrs, fmv_instructions)
    handle_register_reset(state) if should_reset_registers?(state)
  end

  handle_final_checks(state)
  state[:success] ? 0 : 1
end

private

def process_line(state, line, nan_ratio, target_instructions, mattrs, fmv_instructions)
  return if skip_line?(state, line)

  if state[:previous_line]
    process_instruction_pair(state, line, nan_ratio, target_instructions, mattrs, fmv_instructions)
    state[:previous_line] = nil
  else
    state[:previous_line] = line
  end
end

def skip_line?(state, line)
  state[:parsing_started] ||= line.include?("Simulation Start")
  !state[:parsing_started] || line.include?("Simulation Start")
end

def process_instruction_pair(state, line, nan_ratio, target_instructions, mattrs, fmv_instructions)
  parsed_data = parse_instruction_lines(state[:previous_line], line)
  parsed_data ? handle_parsed_data(state, parsed_data, nan_ratio, target_instructions, mattrs, fmv_instructions) : (state[:success] = false)
end

def handle_parsed_data(state, data, nan_ratio, target_instructions, mattrs, fmv_instructions)
  instruction = data[:instruction_name]
  register = data[:register_name]
  value = data[:register_value]

  if state[:nan_ratio_reached]
    handle_nan_ratio_case(state, instruction, register, value, fmv_instructions)
  else
    handle_normal_case(state, instruction, register, value, nan_ratio, target_instructions, mattrs, fmv_instructions)
  end
end

def handle_normal_case(state, instruction, register, value, nan_ratio, target_instructions, mattrs, fmv_instructions)
  if fmv_instruction?(instruction, register, state[:registers], fmv_instructions)
    handle_fmv_instruction(state, register)
  elsif target_instruction?(instruction, target_instructions, mattrs)
    update_registers(state, register, value)
    check_nan_ratio(state, nan_ratio)
  end
end

def handle_nan_ratio_case(state, instruction, register, value, fmv_instructions)
  return unless fmv_instruction?(instruction, nil, nil, fmv_instructions)

  if state[:registers].key?(register)
    state[:registers][register] = value
    state[:found_fmv_count] += 1
  end
end

# Helper methods
def fmv_instruction?(instruction, register, registers, fmv_instructions)
  fmv_instructions.any? { |fmv| instruction.include?(fmv) } &&
    (register.nil? || registers&.key?(register))
end

def target_instruction?(instruction, target_instructions, mattrs)
  target_instructions.include?(instruction) &&
    (instruction.end_with?(".s") && mattrs.include?("+d") || instruction.end_with?(".h"))
end

def update_registers(state, register, value)
  state[:register_count] += 1 unless state[:registers].key?(register)
  state[:registers][register] = value
end

def check_nan_ratio(state, nan_ratio)
  return unless (state[:register_count].to_f / 32) >= nan_ratio
  
  state[:nan_ratio_reached] = true
  state[:fmv_count] = state[:register_count]
end

def handle_fmv_instruction(state, register)
  state[:registers].delete(register)
  state[:register_count] -= 1
end

def should_reset_registers?(state)
  state[:nan_ratio_reached] && state[:found_fmv_count] == state[:fmv_count]
end

def handle_register_reset(state)
  puts "Curr registers = #{state[:registers]}"
  puts "All registers have been overwritten. Reseting now for new tracking."
  state[:registers].clear
  state[:register_count] = state[:fmv_count] = state[:found_fmv_count] = 0
  state[:nan_ratio_reached] = false
end

def handle_final_checks(state)
  return unless state[:nan_ratio_reached] && state[:found_fmv_count] != state[:fmv_count]

  puts "Error: Not all registers had corresponding fmv instruction to get values set"
  puts "Registers: #{state[:registers]}"
  state[:success] = false
end

options = {}

opt_parser = OptionParser.new do |opts|
  opts.banner = "Usage: ruby my_script.rb [options] filename nan_ratio"

  opts.on("-a", "--mattrs INSTRUCTIONS", "List of mattrs") do |i|
    options[:mattrs] = i.split(",")
  end

  opts.on("-i", "--instructions INSTRUCTIONS", "List of instructions to track 
          (comma-separated, e.g., fdiv.s,fdiv.h,fmul.d)") do |i|
    options[:instructions] = i.split(",")
  end

  opts.on("-h", "--help", "Prints this help message") do
    puts opts
    exit
  end
end

begin
  opt_parser.parse!
rescue OptionParser::MissingArgument => e
  puts "Error: " + e.message
  puts opt_parser
  exit 1
rescue OptionParser::InvalidOption => e
  puts "Error: " + e.message
  puts opt_parser
  exit 1
end

# Check for positional arguments: filename and nan_ratio
if ARGV.length < 2 || options[:instructions].nil?
  puts "Error: Please provide filename, nan_ratio, and instructions."
  puts opt_parser
  exit 1
end

filename = ARGV[0]
nan_ratio = ARGV[1].to_f
target_instructions = options[:instructions]
mattrs = options[:mattrs]

# Validate nan_ratio (0.0 to 1.0)
unless nan_ratio >= 0.0 && nan_ratio <= 1.0
  puts "Error: nan ratio must be between 0.0 and 1.0"
  exit 1
end

exit_code = process_file(filename, nan_ratio, target_instructions, mattrs)
exit exit_code

