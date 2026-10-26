#ifndef slic3r_PlaceholderParserOrca_hpp_
#define slic3r_PlaceholderParserOrca_hpp_

#include "PlaceholderParser.hpp"

namespace Slic3r { namespace orca_macro {

// Evaluate `templ` with the OrcaSlicer / PrusaSlicer 2.6 macro language (local/global variables,
// writable output variables such as position[], one_of, interpolate_table, ...). Variables written
// by the script are stored in `config_outputs` when it is given. Throws PlaceholderParserError.
std::string process(const PlaceholderParser &parser, const std::string &templ, unsigned int current_extruder_id,
                    const DynamicConfig *config_override, DynamicConfig *config_outputs, PlaceholderParser::ContextData *context_data);

}} // namespace Slic3r::orca_macro

#endif // slic3r_PlaceholderParserOrca_hpp_
