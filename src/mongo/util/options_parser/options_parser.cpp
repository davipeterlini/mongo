/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/util/options_parser/options_parser.h"

#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <cerrno>
#include <fstream>
#include <stdio.h>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/constraints.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/scopeguard.h"
#include "third_party/yaml-cpp-0.5.1/include/yaml-cpp/yaml.h"

namespace mongo {
namespace optionenvironment {

    using namespace std;

    namespace po = boost::program_options;

    namespace {

        // The following section contains utility functions that convert between the various objects
        // we need to deal with while parsing command line options.
        //
        // These conversions are different depending on the data source because our current
        // implementation uses boost::program_options for the command line and INI files and the
        // yaml-cpp YAML parser for YAML config files.  Our destination storage in both cases is an
        // Environment which stores Value objects.
        //
        // 1. YAML Config Files
        //     The YAML parser parses a YAML config file into a YAML::Node.  Therefore, we need:
        //         a. A function to convert a YAML::Node to a Value (YAMLNodeToValue)
        //         b. A function to iterate a YAML::Node, convert the leaf Nodes to Values, and add
        //            them to our Environment (addYAMLNodesToEnvironment)
        //
        // 2. INI Config Files and command line
        //     The boost::program_options parsers store their output in a
        //     boost::program_options::variables_map.  Therefore, we need:
        //         a. A function to convert a boost::any to a Value (boostAnyToValue)
        //         b. A function to iterate a variables_map, convert the boost::any elements to
        //            Values, and add them to our Environment (addBoostVariablesToEnvironment)

        // Convert a boost::any to a Value.  See comments at the beginning of this section.
        Status boostAnyToValue(const boost::any& anyValue, Value* value) {
            try {
                if (anyValue.type() == typeid(std::vector<std::string>)) {
                    *value = Value(boost::any_cast<std::vector<std::string> >(anyValue));
                }
                else if (anyValue.type() == typeid(bool)) {
                    *value = Value(boost::any_cast<bool>(anyValue));
                }
                else if (anyValue.type() == typeid(double)) {
                    *value = Value(boost::any_cast<double>(anyValue));
                }
                else if (anyValue.type() == typeid(int)) {
                    *value = Value(boost::any_cast<int>(anyValue));
                }
                else if (anyValue.type() == typeid(long)) {
                    *value = Value(boost::any_cast<long>(anyValue));
                }
                else if (anyValue.type() == typeid(std::string)) {
                    *value = Value(boost::any_cast<std::string>(anyValue));
                }
                else if (anyValue.type() == typeid(unsigned long long)) {
                    *value = Value(boost::any_cast<unsigned long long>(anyValue));
                }
                else if (anyValue.type() == typeid(unsigned)) {
                    *value = Value(boost::any_cast<unsigned>(anyValue));
                }
                else {
                    StringBuilder sb;
                    sb << "Unrecognized type: " << anyValue.type().name() <<
                        " in any to Value conversion";
                    return Status(ErrorCodes::InternalError, sb.str());
                }
            }
            catch(const boost::bad_any_cast& e) {
                StringBuilder sb;
                // We already checked the type, so this is just a sanity check
                sb << "boost::any_cast threw exception: " << e.what();
                return Status(ErrorCodes::InternalError, sb.str());
            }
            return Status::OK();
        }

        // Convert a YAML::Node to a Value.  See comments at the beginning of this section.
        Status YAMLNodeToValue(const YAML::Node& YAMLNode,
                               const std::vector<OptionDescription>& options_vector,
                               const Key& key, Value* value) {

            bool isRegistered = false;

            // The logic below should ensure that we don't use this uninitialized, but we need to
            // initialize it here to avoid a compiler warning.  Initializing it to a "Bool" since
            // that's the most restricted type we have and is most likely to result in an early
            // failure if we have a logic error.
            OptionType type = Bool;

            // Get expected type
            for (std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {
                if (key == iterator->_dottedName && (iterator->_sources & SourceYAMLConfig)) {
                    isRegistered = true;
                    type = iterator->_type;
                }
            }

            if (!isRegistered) {
                StringBuilder sb;
                sb << "Unrecognized option: " << key;
                return Status(ErrorCodes::BadValue, sb.str());
            }

            // Handle multi keys
            if (type == StringVector) {
                if (!YAMLNode.IsSequence()) {
                    StringBuilder sb;
                    sb << "Option: " << key
                       << " is of type StringVector, but value in YAML config is not a list type";
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                std::vector<std::string> stringVector;
                for (YAML::const_iterator it = YAMLNode.begin(); it != YAMLNode.end(); ++it) {
                    if (it->IsSequence()) {
                        StringBuilder sb;
                        sb << "Option: " << key
                           << " has nested lists, which is not allowed";
                        return Status(ErrorCodes::BadValue, sb.str());
                    }
                    stringVector.push_back(it->Scalar());
                }
                *value = Value(stringVector);
                return Status::OK();
            }

            Status ret = Status::OK();
            std::string stringVal = YAMLNode.Scalar();
            switch (type) {
                double doubleVal;
                int intVal;
                long longVal;
                unsigned long long unsignedLongLongVal;
                unsigned unsignedVal;
                case Switch:
                case Bool:
                    if (stringVal == "true") {
                        *value = Value(true);
                        return Status::OK();
                    }
                    else if (stringVal == "true") {
                        *value = Value(false);
                        return Status::OK();
                    }
                    else {
                        StringBuilder sb;
                        sb << "Expected boolean but found string: " << stringVal
                           << " for option: " << key;
                        return Status(ErrorCodes::BadValue, sb.str());
                    }
                case Double:
                    ret = parseNumberFromString(stringVal, &doubleVal);
                    if (!ret.isOK()) {
                        return ret;
                    }
                    *value = Value(doubleVal);
                    return Status::OK();
                case Int:
                    ret = parseNumberFromString(stringVal, &intVal);
                    if (!ret.isOK()) {
                        return ret;
                    }
                    *value = Value(intVal);
                    return Status::OK();
                case Long:
                    ret = parseNumberFromString(stringVal, &longVal);
                    if (!ret.isOK()) {
                        return ret;
                    }
                    *value = Value(longVal);
                    return Status::OK();
                case String:
                    *value = Value(stringVal);
                    return Status::OK();
                case UnsignedLongLong:
                    ret = parseNumberFromString(stringVal, &unsignedLongLongVal);
                    if (!ret.isOK()) {
                        return ret;
                    }
                    *value = Value(unsignedLongLongVal);
                    return Status::OK();
                case Unsigned:
                    ret = parseNumberFromString(stringVal, &unsignedVal);
                    if (!ret.isOK()) {
                        return ret;
                    }
                    *value = Value(unsignedVal);
                    return Status::OK();
                default: /* XXX: should not get here */
                    return Status(ErrorCodes::InternalError, "Unrecognized option type");
            }
        }

        // Add all the values in the given variables_map to our environment.  See comments at the
        // beginning of this section.
        Status addBoostVariablesToEnvironment(const po::variables_map& vm,
                                              const OptionSection& options,
                                              Environment* environment) {

            std::vector<OptionDescription> options_vector;
            Status ret = options.getAllOptions(&options_vector);
            if (!ret.isOK()) {
                return ret;
            }

            for(std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {

                // Trim off the short option from our name so we can look it up correctly in our map
                std::string long_name;
                std::string::size_type commaOffset = iterator->_singleName.find(',');
                if (commaOffset != string::npos) {
                    if (commaOffset != iterator->_singleName.size() - 2) {
                        StringBuilder sb;
                        sb << "Unexpected comma in option name: \"" << iterator->_singleName << "\""
                           << ": option name must be in the format \"option,o\" or \"option\", "
                           << "where \"option\" is the long name and \"o\" is the optional one "
                           << "character short alias";
                        return Status(ErrorCodes::BadValue, sb.str());
                    }
                    long_name = iterator->_singleName.substr(0, commaOffset);
                } else {
                    long_name = iterator->_singleName;
                }

                if (vm.count(long_name)) {
                    Value optionValue;
                    Status ret = boostAnyToValue(vm[long_name].value(), &optionValue);
                    if (!ret.isOK()) {
                        return ret;
                    }

                    // XXX: Don't set switches that are false, to maintain backwards compatibility
                    // with the old behavior during the transition to the new parser
                    if (iterator->_type == Switch) {
                        bool value;
                        ret = optionValue.get(&value);
                        if (!ret.isOK()) {
                            return ret;
                        }
                        if (!value) {
                            continue;
                        }
                    }

                    environment->set(iterator->_dottedName, optionValue);
                }
            }
            return Status::OK();
        }

        // Add all the values in the given YAML Node to our environment.  See comments at the
        // beginning of this section.
        Status addYAMLNodesToEnvironment(const YAML::Node& root,
                                         const OptionSection& options,
                                         const std::string parentPath,
                                         Environment* environment) {

            std::vector<OptionDescription> options_vector;
            Status ret = options.getAllOptions(&options_vector);
            if (!ret.isOK()) {
                return ret;
            }

            // Don't return an error on empty config files
            if (root.IsNull()) {
                return Status::OK();
            }

            if (!root.IsMap() && parentPath.empty()) {
                StringBuilder sb;
                sb << "No map found at top level of YAML config";
                return Status(ErrorCodes::BadValue, sb.str());
            }

            for (YAML::const_iterator it = root.begin(); it != root.end(); ++it) {
                std::string fieldName = it->first.Scalar();
                YAML::Node YAMLNode = it->second;

                std::string dottedName;
                if (parentPath.empty()) {

                    // We are at the top level, so the full specifier is just the current field name
                    dottedName = fieldName;
                }
                else {

                    // If our field name is "value", assume this contains the value for the parent
                    if (fieldName == "value") {
                        dottedName = parentPath;
                    }

                    // If this is not a special field name, and we are in a sub object, append our
                    // current fieldName to the selector for the sub object we are traversing
                    else {
                        dottedName = parentPath + '.' + fieldName;
                    }
                }

                if (YAMLNode.IsMap()) {
                    addYAMLNodesToEnvironment(YAMLNode, options, dottedName, environment);
                }
                else {
                    Value optionValue;
                    Status ret = YAMLNodeToValue(YAMLNode, options_vector, dottedName,
                                                 &optionValue);
                    if (!ret.isOK()) {
                        return ret;
                    }

                    Value dummyVal;
                    if (environment->get(dottedName, &dummyVal).isOK()) {
                        StringBuilder sb;
                        sb << "Error parsing YAML config: duplcate key: " << dottedName;
                        return Status(ErrorCodes::BadValue, sb.str());
                    }

                    ret = environment->set(dottedName, optionValue);
                    if (!ret.isOK()) {
                        return ret;
                    }
                }
            }

            return Status::OK();
        }

        /**
        * For all options that we registered as composable, combine the values from source and dest
        * and set the result in dest.  Note that this only works for options that are registered as
        * vectors of strings.
        */
        Status addCompositions(const OptionSection& options,
                               const Environment& source,
                               Environment* dest) {
            std::vector<OptionDescription> options_vector;
            Status ret = options.getAllOptions(&options_vector);
            if (!ret.isOK()) {
                return ret;
            }

            for(std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {
                if (iterator->_isComposing) {
                    std::vector<std::string> source_value;
                    ret = source.get(iterator->_dottedName, &source_value);
                    if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                        StringBuilder sb;
                        sb << "Error getting composable vector value from source: "
                            << ret.toString();
                        return Status(ErrorCodes::InternalError, sb.str());
                    }
                    // Only do something if our source environment has something to add
                    else if (ret.isOK()) {
                        std::vector<std::string> dest_value;
                        ret = dest->get(iterator->_dottedName, &dest_value);
                        if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                            StringBuilder sb;
                            sb << "Error getting composable vector value from dest: "
                                << ret.toString();
                            return Status(ErrorCodes::InternalError, sb.str());
                        }

                        // Append source_value on the end of dest_value
                        dest_value.insert(dest_value.end(),
                                          source_value.begin(),
                                          source_value.end());

                        // Set the resulting value in our output environment
                        ret = dest->set(Key(iterator->_dottedName), Value(dest_value));
                        if (!ret.isOK()) {
                            return ret;
                        }
                    }
                }
            }

            return Status::OK();
        }

        /**
        * For all options that have constraints, add those constraints to our environment so that
        * they run when the environment gets validated.
        */
        Status addConstraints(const OptionSection& options, Environment* dest) {
            std::vector<boost::shared_ptr<Constraint> > constraints_vector;

            Status ret = options.getConstraints(&constraints_vector);
            if (!ret.isOK()) {
                return ret;
            }

            std::vector<boost::shared_ptr<Constraint> >::const_iterator citerator;
            for (citerator = constraints_vector.begin();
                    citerator != constraints_vector.end(); citerator++) {
                dest->addConstraint(citerator->get());
            }

            return Status::OK();
        }

    } // namespace

    /**
     * This function delegates the command line parsing to boost program_options.
     *
     * 1. Extract the boost readable option descriptions and positional option descriptions from the
     * OptionSection
     * 2. Passes them to the boost command line parser
     * 3. Copy everything from the variables map returned by boost into the Environment
     */
    Status OptionsParser::parseCommandLine(const OptionSection& options,
                                           const std::vector<std::string>& argv,
                                           Environment* environment) {
        po::options_description boostOptions;
        po::positional_options_description boostPositionalOptions;
        po::variables_map vm;

        // Need to convert to an argc and a vector of char* since that is what
        // boost::program_options expects as input to its command line parser
        int argc = 0;
        std::vector<const char*> argv_buffer;
        for (std::vector<std::string>::const_iterator iterator = argv.begin();
            iterator != argv.end(); iterator++) {
            argv_buffer.push_back(iterator->c_str());
            argc++;
        }

        /**
         * Style options for boost command line parser
         *
         * unix_style is an alias for a group of basic style options.  We are deviating from that
         * base style in the following ways:
         *
         * 1. Don't allow guessing - '--dbpat' != '--dbpath'
         * 2. Don't allow sticky - '-hf' != '-h -f'
         * 3. Allow long disguises - '--dbpath' == '-dbpath'
         *
         * In some executables, we are using multiple 'v' options to set the verbosity (e.g. '-vvv')
         * To make this work, we need to allow long disguises and disallow guessing.
         */
        int style = (((po::command_line_style::unix_style ^
                       po::command_line_style::allow_guessing) |
                      po::command_line_style::allow_long_disguise) ^
                     po::command_line_style::allow_sticky);

        Status ret = options.getBoostOptions(&boostOptions, false, false, SourceCommandLine);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options.getBoostPositionalOptions(&boostPositionalOptions);
        if (!ret.isOK()) {
            return ret;
        }

        try {
            po::store(po::command_line_parser(argc, &argv_buffer[0]).
                      options(boostOptions).
                      positional(boostPositionalOptions).
                      style(style).
                      run(), vm);

            ret = addBoostVariablesToEnvironment(vm, options, environment);
            if (!ret.isOK()) {
                return ret;
            }
        }
        catch (po::multiple_occurrences& e) {
            StringBuilder sb;
            sb << "Error parsing command line:  Multiple occurrences of option \"--"
               << e.get_option_name() << "\"";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        catch (po::error& e) {
            StringBuilder sb;
            sb << "Error parsing command line: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        }
        return Status::OK();
    }

    /**
     * This function delegates the INI config parsing to boost program_options.
     *
     * 1. Extract the boost readable option descriptions from the OptionSection
     * 2. Passes them to the boost config file parser
     * 3. Copy everything from the variables map returned by boost into the Environment
     */
    Status OptionsParser::parseINIConfigFile(const OptionSection& options,
                                             const std::string& config,
                                             Environment* environment) {
        po::options_description boostOptions;
        po::variables_map vm;

        Status ret = options.getBoostOptions(&boostOptions, false, false, SourceINIConfig);
        if (!ret.isOK()) {
            return ret;
        }

        std::istringstream is(config);
        try {
            po::store(po::parse_config_file(is, boostOptions), vm);
            ret = addBoostVariablesToEnvironment(vm, options, environment);
            if (!ret.isOK()) {
                return ret;
            }
        }
        catch (po::multiple_occurrences& e) {
            StringBuilder sb;
            sb << "Error parsing INI config file:  Multiple occurrences of option \""
               << e.get_option_name() << "\"";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        catch (po::error& e) {
            StringBuilder sb;
            sb << "Error parsing INI config file: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        }
        return Status::OK();
    }

namespace {

    /**
     * This function delegates the YAML config parsing to the third party YAML parser.  It does no
     * error checking other than the parse error checking done by the YAML parser.
     */
    Status parseYAMLConfigFile(const std::string& config,
                               YAML::Node* YAMLConfig) {

        try {
            *YAMLConfig = YAML::Load(config);
        } catch (const YAML::Exception &e) {
            StringBuilder sb;
            sb << "Error parsing YAML config file: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        } catch (const std::runtime_error &e) {
            StringBuilder sb;
            sb << "Unexpected exception parsing YAML config file: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        }

        return Status::OK();
    }

    bool isYAMLConfig(const YAML::Node& config) {
        // The YAML parser is very forgiving, and for the INI config files we've parsed so far using
        // the YAML parser, the YAML parser just slurps the entire config file into a single string
        // rather than erroring out.  Thus, we assume that having a scalar (string) as the root node
        // means that this is not meant to be a YAML config file, since even a very simple YAML
        // config file should be parsed as a Map, and thus "config.IsScalar()" would return false.
        //
        // This requires more testing, both to ensure that all INI style files get parsed as a
        // single string, and to ensure that the user experience does not suffer (in terms of this
        // causing confusing error messages for users writing a brand new YAML config file that
        // incorrectly triggers this check).
        if (config.IsScalar()) {
            return false;
        }
        else {
            return true;
        }
    }

} // namespace

    /**
     * Add default values from the given OptionSection to the given Environment
     */
    Status OptionsParser::addDefaultValues(const OptionSection& options,
                                           Environment* environment) {
        std::map <Key, Value> defaultOptions;

        Status ret = options.getDefaults(&defaultOptions);
        if (!ret.isOK()) {
            return ret;
        }

        typedef std::map<Key, Value>::iterator it_type;
        for(it_type iterator = defaultOptions.begin();
            iterator != defaultOptions.end(); iterator++) {
            ret = environment->setDefault(iterator->first, iterator->second);
            if (!ret.isOK()) {
                return ret;
            }
        }

        return Status::OK();
    }

    /**
     * Reads the entire config file into the output string.  This was done this way because the JSON
     * parser only takes complete strings, and we were using that to parse the config file before.
     * We could redesign the parser to use some kind of streaming interface, but for now this is
     * simple and works for the current use case of config files which should be limited in size.
     */
    Status OptionsParser::readConfigFile(const std::string& filename, std::string* contents) {

        FILE* config;
        config = fopen(filename.c_str(), "r");
        if (config == NULL) {
            const int current_errno = errno;
            StringBuilder sb;
            sb << "Error reading config file: " << strerror(current_errno);
            return Status(ErrorCodes::InternalError, sb.str());
        }
        ON_BLOCK_EXIT(fclose, config);

        // Get length of config file by seeking to the end and getting the cursor position
        if (fseek(config, 0L, SEEK_END) != 0) {
            const int current_errno = errno;
            // TODO: Make sure errno is the correct way to do this
            // Confirmed that errno gets set in Mac OSX, but not documented
            StringBuilder sb;
            sb << "Error seeking in config file: " << strerror(current_errno);
            return Status(ErrorCodes::InternalError, sb.str());
        }
        long configSize = ftell(config);

        // Seek back to the beginning of the file for reading
        if (fseek(config, 0L, SEEK_SET) != 0) {
            const int current_errno = errno;
            // TODO: Make sure errno is the correct way to do this
            // Confirmed that errno gets set in Mac OSX, but not documented
            StringBuilder sb;
            sb << "Error seeking in config file: " << strerror(current_errno);
            return Status(ErrorCodes::InternalError, sb.str());
        }

        // Read into a vector first since it's guaranteed to have contiguous storage
        std::vector<char> configVector;
        configVector.resize(configSize);

        if (configSize > 0) {
            long nread = 0;
            while (!feof(config) && nread < configSize) {
                nread = nread + fread(&configVector[nread], sizeof(char),
                                      configSize - nread, config);
                if (ferror(config)) {
                    const int current_errno = errno;
                    // TODO: Make sure errno is the correct way to do this
                    StringBuilder sb;
                    sb << "Error reading in config file: " << strerror(current_errno);
                    return Status(ErrorCodes::InternalError, sb.str());
                }
            }
            // Resize our config vector to the number of bytes we actually read
            configVector.resize(nread);
        }

        // Copy the vector contents into our result string
        *contents = std::string(configVector.begin(), configVector.end());

        return Status::OK();
    }

    /**
     * Run the OptionsParser
     *
     * Overview:
     *
     * 1. Parse argc and argv using the given OptionSection as a description of expected options
     * 2. Check for a "config" argument
     * 3. If "config" found, read config file
     * 4. Detect config file type (YAML or INI)
     * 5. Parse config file using the given OptionSection as a description of expected options
     * 6. Add the results to the output Environment in the proper order to ensure correct precedence
     */
    Status OptionsParser::run(const OptionSection& options,
            const std::vector<std::string>& argv,
            const std::map<std::string, std::string>& env, // XXX: Currently unused
            Environment* environment) {

        Environment commandLineEnvironment;
        Environment configEnvironment;
        Environment composedEnvironment;

        Status ret = parseCommandLine(options, argv, &commandLineEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        Value config_value;
        ret = commandLineEnvironment.get(Key("config"), &config_value);
        // We had an error other than "config" not existing in our environment
        if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
            return ret;
        }
        // "config" exists in our environment
        else if (ret.isOK()) {

            // Environment::get returns a bad status if config was not set
            std::string config_filename;
            ret = config_value.get(&config_filename);
            if (!ret.isOK()) {
                return ret;
            }

            std::string config_file;
            ret = readConfigFile(config_filename, &config_file);
            if (!ret.isOK()) {
                return ret;
            }

            YAML::Node YAMLConfig;
            ret = parseYAMLConfigFile(config_file, &YAMLConfig);
            if (!ret.isOK()) {
                return ret;
            }

            if (isYAMLConfig(YAMLConfig)) {
                ret = addYAMLNodesToEnvironment(YAMLConfig, options, "", &configEnvironment);
                if (!ret.isOK()) {
                    return ret;
                }
            }
            else {
                ret = parseINIConfigFile(options, config_file, &configEnvironment);
                if (!ret.isOK()) {
                    return ret;
                }
            }
        }

        // Adds the values for all our options that were registered as composable to the composed
        // environment.  addCompositions doesn't override the values like "setAll" on our
        // environment.  Instead it aggregates the values in the result environment.
        ret = addCompositions(options, commandLineEnvironment, &composedEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addCompositions(options, configEnvironment, &composedEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        // Add the default values to our resulting environment
        ret = addDefaultValues(options, environment);
        if (!ret.isOK()) {
            return ret;
        }

        // Add the values to our result in the order of override
        // NOTE: This should not fail validation as we haven't called environment->validate() yet
        ret = environment->setAll(configEnvironment);
        if (!ret.isOK()) {
            return ret;
        }
        ret = environment->setAll(commandLineEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        // Add this last because it represents the aggregated results of composing all environments
        ret = environment->setAll(composedEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        // Add the constraints from our options to the result environment
        ret = addConstraints(options, environment);
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

} // namespace optionenvironment
} // namespace mongo
