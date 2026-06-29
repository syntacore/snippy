#!/usr/bin/env ruby
# frozen_string_literal: true

# --------------------------------------------------------------
# generate_isa_tests.rb – Execute `'isa-tests-gen.sh'` for each test described
#                     in a YAML file and write the script’s stdout
#                     to a file derived from the YAML.
#
#   ruby generate_isa_tests.rb --config path/to/tests.yml
#
#   Options
#     -c, --config FILE               Path to the YAML file (required)
#     -o, --output-directory DIR      Base directory for all output files.
#                                     Default: current working directory.
#     -h, --help                      Show this help message.
#
#   Expected YAML shape
#   -----------------------------------------------------------
#   tests:
#   - filename:              output.yaml         # name relative to the output directory
#     description:           test description    # optional
#     march:                 rv64gc_aaa
#     mtriple:               riscv64
#     ignore_opcodes_regexp: CADD*               # optional
#     num_instructions:      128                 # optional
#     include:                                   # optional
#       - inc1.yaml
#       - inc2.yaml
#   -----------------------------------------------------------
#
#   For each entry the script invokes `isa-tests-gen.sh` (located next to this
#   Ruby file) with required arguments.
#   The stdout of `isa-tests-gen.sh` is written to:
#
#       File.join(output_directory, entry['filename'])
#
# --------------------------------------------------------------

require 'optparse'
require 'yaml'
require 'shellwords'
require 'open3'
require 'set'

# ------------------------------------------------------------------
# Helper methods
# ------------------------------------------------------------------
def abort_with(message, status = 1)
  $stderr.puts "Error: #{message}"
  exit status
end

def info(message)
  $stdout.puts "Info: #{message}"
end

# Returns a hash that must contain :config (the YAML file path).
def parse_options
  options = {
    output_directory: '.' # default = current working directory
  }
  parser  = OptionParser.new do |opts|
    opts.banner = <<~BANNER
      Usage:
        #{File.basename($PROGRAM_NAME)} [options]

      Options:
    BANNER

    opts.on("-c", "--config FILE", "Path to the YAML file (required)") do |file|
      options[:config] = file
    end

    opts.on("-o", "--output-directory DIR",
            "Base directory where test results will be stored (default: '.')") do |d|
      options[:output_directory] = d
    end

    opts.on("-h", "--help", "Show this help message") do
      puts opts
      exit
    end

  end

  begin
    parser.parse!
  rescue OptionParser::InvalidOption, OptionParser::MissingArgument => e
    abort_with e.message
  end

  # Enforce presence of the required option.
  unless options[:config]
    abort_with <<~MSG
      Missing required option '--config'.
      #{parser}
    MSG
  end

  options
end

def validate_file(path)
  unless File.file?(path) && File.readable?(path)
    abort_with "Cannot read file '#{path}'. Check that the path exists and is readable."
  end
end

def load_yaml(path)
  YAML.safe_load_file(
    path,
    permitted_classes: [],   # disallow arbitrary Ruby objects
    permitted_symbols: [],   # disallow symbols unless you explicitly allow them
    aliases: true            # allow anchors / aliases (safe in recent Ruby)
  )
rescue Psych::SyntaxError => e
  abort_with "YAML syntax error in '#{path}':\n#{e.message}"
rescue => e
  abort_with "Failed to load YAML: #{e.class} – #{e.message}"
end

# ------------------------------------------------------------------
# Validate a single test entry (required keys)
# ------------------------------------------------------------------
REQUIRED_KEYS = %w[filename march mtriple extensions].freeze

def validate_entry(entry)
  missing = REQUIRED_KEYS.reject { |k| entry.key?(k) }
  return if missing.empty?
  abort_with "Missing required key(s) #{missing.map { |k| "'#{k}'" }.join(', ')} in test entry:\n#{entry.inspect}"
end

def validate_tests_config_file(tests)
  seen   = Set.new
  dupes  = Set.new
  tests.each do |entry|
    # Validate required keys once for each entry.
    validate_entry(entry)

    filename = entry['filename']
    if seen.include?(filename)
      dupes << filename
    else
      seen << filename
    end
  end
  unless dupes.empty?
    abort_with "Duplicate output file(s) detected:\n  #{dupes.to_a.join("\n  ")}\n" \
               "Each test's `filename` must be unique."
  end
end

# ------------------------------------------------------------------
# Build the command that will be executed for a single test
# ------------------------------------------------------------------
def build_command(entry, script_path)
  # Optional keys – we keep them only when they have meaningful values.
  ignore_regexp = entry['ignore-opcode-regex']     # may be nil
  num_instructions = entry['num_instructions']     # may be nil
  includes_arr     = entry['include']              # expected to be an Array (or nil)
  extra_options    = entry['extra-options']        # expected to be a string but may bit nil

  cmd = [
    script_path,
    '--march',        entry['march'],
    '--mtriple',   entry['mtriple'],
    '--extensions', entry['extensions']
  ]

  # Append optional argument only when present.
  cmd += ['--ignore-opcode-regex', ignore_regexp] if ignore_regexp
  # Add --num-instructions only if the key exists (any integer/string is accepted)
  cmd += ['--num-instrs', num_instructions.to_s] if num_instructions
  # Pass additional options if defined
  cmd += ['--extra-options', extra_options] if extra_options

  if includes_arr
    abort_with "'include' must be an Array (got #{includes_arr.class})" unless includes_arr.is_a?(Array)

    includes_arr.each do |inc|
      cmd += ['--include', inc.to_s]
    end
  end

  cmd
end

# ------------------------------------------------------------------
# Run a single test and write its stdout to the final file
# ------------------------------------------------------------------
def run_command(entry, cmd, output_dir, continue_on_error:)
  relative_name = entry['filename']
  final_out_path    = File.join(output_dir, relative_name)
  info "Running: #{cmd.shelljoin}"
  stdout, stderr, status = Open3.capture3(*cmd)

  # Write the captured stdout to the file specified in the YAML.
  begin
    File.open(final_out_path, 'w') { |f| f.write(stdout) }
  rescue => e
    abort_with "Failed to write to '#{out_file}': #{e.class} – #{e.message}"
  end

  $stderr.print stderr unless stderr.empty?

  if status.success?
    info "✅ Success"
    true
  else
    msg = "❌ Command failed (exit #{status.exitstatus})"
    abort_with msg, status.exitstatus
  end
end

def run
  opts   = parse_options
  yaml_path   = opts[:config]
  validate_file(yaml_path)
  data = load_yaml(yaml_path)
  tests = data['tests']
  abort_with "YAML file does not contain a top-level 'tests' array." unless tests.is_a?(Array)
  validate_tests_config_file(tests)
  script_path = File.expand_path('isa-tests-gen.sh', __dir__)
  abort_with "Unable to locate isa-tests-gen.sh at '#{script_path}'." unless File.executable?(script_path)

  tests.each_with_index do |entry, idx|
    info "=== Test ##{idx + 1} (#{entry['filename']}) ==="
    desc = entry['description']
    info "=== #{desc}" if desc
    cmd = build_command(entry, script_path)
    run_command(entry, cmd, opts[:output_directory], continue_on_error: opts[:continue_on_error])
  end
end

run if __FILE__ == $PROGRAM_NAME
