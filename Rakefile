# -*- ruby -*-
require 'rake/extensiontask'
require 'rspec/core/rake_task'
require 'rubocop/rake_task'
require 'bundler/gem_tasks'
require 'fileutils'

require_relative 'build_config.rb'

load 'tools/distrib/rake_compiler_docker_image.rb'

# Add rubocop style checking tasks
RuboCop::RakeTask.new(:rubocop) do |task|
  task.options = ['-c', 'src/ruby/.rubocop.yml']
  # add end2end tests to formatter but don't add generated proto _pb.rb's
  task.patterns = ['src/ruby/{lib,spec}/**/*.rb', 'src/ruby/end2end/*.rb']
end

spec = Gem::Specification.load('grpc.gemspec')

Gem::PackageTask.new(spec) do |pkg|
end

# Add the extension compiler task
Rake::ExtensionTask.new('grpc_c', spec) do |ext|
  ext.source_pattern = '**/*.{c,h}'
  ext.ext_dir = File.join('src', 'ruby', 'ext', 'grpc')
  ext.lib_dir = File.join('src', 'ruby', 'lib', 'grpc')
  ext.cross_compile = true
  ext.cross_platform = [
    'x86-mingw32', 'x64-mingw32',
    'x86_64-linux', 'x86-linux',
    'x86_64-darwin',
    'universal-darwin'
  ]
  ext.cross_compiling do |spec|
    spec.files = %w( etc/roots.pem grpc_c.32.ruby grpc_c.64.ruby )
    spec.files += Dir.glob('src/ruby/bin/**/*')
    spec.files += Dir.glob('src/ruby/ext/**/*')
    spec.files += Dir.glob('src/ruby/lib/**/*')
    spec.files += Dir.glob('src/ruby/pb/**/*')
  end
end

CLEAN.add "src/ruby/lib/grpc/[0-9].[0-9]", "src/ruby/lib/grpc/grpc_c.{bundle,so}"

# Define the test suites
SPEC_SUITES = [
  { id: :wrapper, title: 'wrapper layer', files: %w(src/ruby/spec/*.rb) },
  { id: :idiomatic, title: 'idiomatic layer', dir: %w(src/ruby/spec/generic),
    tags: ['~bidi', '~server'] },
  { id: :bidi, title: 'bidi tests', dir: %w(src/ruby/spec/generic),
    tag: 'bidi' },
  { id: :server, title: 'rpc server thread tests', dir: %w(src/ruby/spec/generic),
    tag: 'server' },
  { id: :pb, title: 'protobuf service tests', dir: %w(src/ruby/spec/pb) }
]
namespace :suite do
  SPEC_SUITES.each do |suite|
    desc "Run all specs in the #{suite[:title]} spec suite"
    RSpec::Core::RakeTask.new(suite[:id]) do |t|
      ENV['COVERAGE_NAME'] = suite[:id].to_s
      spec_files = []
      suite[:files].each { |f| spec_files += Dir[f] } if suite[:files]

      if suite[:dir]
        suite[:dir].each { |f| spec_files += Dir["#{f}/**/*_spec.rb"] }
      end
      helper = 'src/ruby/spec/spec_helper.rb'
      spec_files << helper unless spec_files.include?(helper)

      t.pattern = spec_files
      t.rspec_opts = "--tag #{suite[:tag]}" if suite[:tag]
      if suite[:tags]
        t.rspec_opts = suite[:tags].map { |x| "--tag #{x}" }.join(' ')
      end
    end
  end
end

desc 'Build the Windows gRPC DLLs for Ruby'
task 'dlls' do
  grpc_config = ENV['GRPC_CONFIG'] || 'opt'
  verbose = ENV['V'] || '0'

  env = 'CPPFLAGS="-D_WIN32_WINNT=0x600 -DNTDDI_VERSION=0x06000000 -DUNICODE -D_UNICODE -Wno-unused-variable -Wno-unused-result -DCARES_STATICLIB -Wno-error=conversion -Wno-sign-compare -Wno-parentheses -Wno-format -DWIN32_LEAN_AND_MEAN" '
  env += 'CFLAGS="-Wno-incompatible-pointer-types" '
  env += 'CXXFLAGS="-std=c++11 -fno-exceptions" '
  env += 'LDFLAGS=-static '
  env += 'SYSTEM=MINGW32 '
  env += 'EMBED_ZLIB=true '
  env += 'EMBED_OPENSSL=true '
  env += 'EMBED_CARES=true '
  env += 'BUILDDIR=/tmp '
  env += "V=#{verbose} "
  out = GrpcBuildConfig::CORE_WINDOWS_DLL

  w64 = { cross: 'x86_64-w64-mingw32', out: 'grpc_c.64.ruby', platform: 'x64-mingw32' }
  w32 = { cross: 'i686-w64-mingw32', out: 'grpc_c.32.ruby', platform: 'x86-mingw32' }

  [ w64, w32 ].each do |opt|
    env_comp = "CC=#{opt[:cross]}-gcc "
    env_comp += "CXX=#{opt[:cross]}-g++ "
    env_comp += "LD=#{opt[:cross]}-gcc "
    env_comp += "LDXX=#{opt[:cross]}-g++ "
    run_rake_compiler(opt[:platform], <<~EOT)
      gem update --system --no-document && \
      #{env} #{env_comp} make -j`nproc` #{out} && \
      #{opt[:cross]}-strip -x -S #{out} && \
      cp #{out} #{opt[:out]}
    EOT
  end
end

desc 'Build the native gem file under rake_compiler_dock'
task 'gem:native' do
  verbose = ENV['V'] || '0'

  grpc_config = ENV['GRPC_CONFIG'] || 'opt'
  ruby_cc_versions = ['3.0.0', '2.7.0', '2.6.0', '2.5.0', '2.4.0'].join(':')

  if RUBY_PLATFORM =~ /darwin/
    FileUtils.touch 'grpc_c.32.ruby'
    FileUtils.touch 'grpc_c.64.ruby'
    unless '2.5' == /(\d+\.\d+)/.match(RUBY_VERSION).to_s
      fail "rake gem:native (the rake task to build the binary packages) is being " \
        "invoked on macos with ruby #{RUBY_VERSION}. The ruby macos artifact " \
        "build should be running on ruby 2.5."
    end
    system "rake cross native gem RUBY_CC_VERSION=#{ruby_cc_versions} V=#{verbose} GRPC_CONFIG=#{grpc_config}"
  else
    Rake::Task['dlls'].execute
    ['x86-mingw32', 'x64-mingw32'].each do |plat|
      run_rake_compiler(plat, <<~EOT)
        gem update --system --no-document && \
        bundle && \
        bundle exec rake clean && \
        bundle exec rake native:#{plat} pkg/#{spec.full_name}-#{plat}.gem pkg/#{spec.full_name}.gem \
          RUBY_CC_VERSION=#{ruby_cc_versions} \
          V=#{verbose} \
          GRPC_CONFIG=#{grpc_config}
      EOT
    end
    # Truncate grpc_c.*.ruby files because they're for Windows only.
    File.truncate('grpc_c.32.ruby', 0)
    File.truncate('grpc_c.64.ruby', 0)
    ['x86_64-linux', 'x86-linux', 'x86_64-darwin'].each do |plat|
      run_rake_compiler(plat, <<~EOT)
        gem update --system --no-document && \
        bundle && \
        bundle exec rake clean && \
        bundle exec rake native:#{plat} pkg/#{spec.full_name}-#{plat}.gem pkg/#{spec.full_name}.gem \
          RUBY_CC_VERSION=#{ruby_cc_versions} \
          V=#{verbose} \
          GRPC_CONFIG=#{grpc_config}
      EOT
    end
  end
end

# Define dependencies between the suites.
task 'suite:wrapper' => [:compile, :rubocop]
task 'suite:idiomatic' => 'suite:wrapper'
task 'suite:bidi' => 'suite:wrapper'
task 'suite:server' => 'suite:wrapper'
task 'suite:pb' => 'suite:server'

desc 'Compiles the gRPC extension then runs all the tests'
task all: ['suite:idiomatic', 'suite:bidi', 'suite:pb', 'suite:server']
task default: :all
