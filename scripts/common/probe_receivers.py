"""Compare fixed receiver reports without treating missing coverage as black light."""
import math


def _vector3(value):
    if not isinstance(value, list) or len(value) != 3 or any(
        isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in value
    ):
        raise ValueError('Receiver vectors must contain three finite numbers')
    return value


def _luminance(value):
    return sum(a * b for a, b in zip(value, (.2126, .7152, .0722)))


def _metrics(errors):
    if not errors:
        return None
    return {
        'count': len(errors),
        'meanAbsoluteError': sum(errors) / len(errors),
        'rootMeanSquaredError': math.hypot(*errors) / math.sqrt(len(errors)),
        'maximumAbsoluteError': max(errors),
    }


def compare_receivers(before, after, maximum_details=12):
    """Require identical scene, sampling sites and reference irradiance."""
    if type(maximum_details) is not int or not 1 <= maximum_details <= 128:
        raise ValueError('Detail limit must be between 1 and 128')
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise ValueError('Receiver reports must be JSON objects')
    identity = before.get('sceneHash')
    if not isinstance(identity, str) or not identity or identity != after.get('sceneHash'):
        raise ValueError('Receiver reports must describe the same frozen scene')
    if not isinstance(before.get('scope'), str) or not before['scope'] or before['scope'] != after.get('scope'):
        raise ValueError('Receiver measurement scopes differ')
    left, right = before.get('receivers'), after.get('receivers')
    if not isinstance(left, list) or not isinstance(right, list) or len(left) != len(right):
        raise ValueError('Receiver sample counts differ')
    for report in (before, after):
        if type(report.get('sampled')) is not int or report['sampled'] != len(left):
            raise ValueError('Receiver count does not match report metadata')
    errors_before, errors_after, changed = [], [], []
    lost = gained = improved = regressed = 0
    for index, (a, b) in enumerate(zip(left, right)):
        for row in (a, b):
            if not isinstance(row, dict):
                raise ValueError('Receiver samples must be JSON objects')
            for field in ('position', 'normal', 'surface', 'probe'):
                _vector3(row.get(field))
            if type(row.get('covered')) is not bool:
                raise ValueError('Receiver coverage must be explicit')
            for field in ('chart', 'draw'):
                if type(row.get(field)) is not int or row[field] < 0:
                    raise ValueError('Receiver ownership must be explicit')
        if any(a.get(key) != b.get(key) for key in ('chart', 'draw', 'cluster', 'position', 'normal', 'surface')):
            raise ValueError(f'Receiver {index} changed identity or reference irradiance')
        lost += a['covered'] and not b['covered']
        gained += b['covered'] and not a['covered']
        old = new = None
        if a['covered'] and b['covered']:
            reference = _luminance(a['surface'])
            old = abs(_luminance(a['probe']) - reference)
            new = abs(_luminance(b['probe']) - reference)
            errors_before.append(old)
            errors_after.append(new)
            improved += new < old
            regressed += new > old
        if a['covered'] != b['covered'] or a['probe'] != b['probe']:
            changed.append({
                'index': index, 'draw': a['draw'], 'chart': a['chart'],
                'position': a['position'], 'normal': a['normal'], 'reference': a['surface'],
                'coveredBefore': a['covered'], 'coveredAfter': b['covered'],
                'probeBefore': a['probe'], 'probeAfter': b['probe'],
                'errorBefore': old, 'errorAfter': new,
                'errorIncrease': new - old if old is not None else None,
            })
    comparable = [row for row in changed if row['errorIncrease'] is not None]
    return {
        'sceneHash': identity, 'scope': before['scope'], 'sampled': len(left),
        'comparisonScope': 'Common covered receivers only; absolute luminance error, no area weighting or quality threshold',
        'coveredBefore': sum(row['covered'] for row in left),
        'coveredAfter': sum(row['covered'] for row in right),
        'coverageLost': lost, 'coverageGained': gained, 'changed': len(changed),
        'improved': improved, 'regressed': regressed,
        'before': _metrics(errors_before), 'after': _metrics(errors_after),
        'largestRegressions': sorted((row for row in comparable if row['errorIncrease'] > 0),
                                     key=lambda row: (-row['errorIncrease'], row['index']))[:maximum_details],
        'largestImprovements': sorted((row for row in comparable if row['errorIncrease'] < 0),
                                      key=lambda row: (row['errorIncrease'], row['index']))[:maximum_details],
        'coverageChanges': [row for row in changed if row['coveredBefore'] != row['coveredAfter']][:maximum_details],
    }
